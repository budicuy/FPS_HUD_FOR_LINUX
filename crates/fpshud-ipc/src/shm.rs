use crate::protocol::{FrameBackendKind, ShmSlotMeta};
use rustix::fd::{AsFd, BorrowedFd};
use rustix::fs::{MemfdFlags, ftruncate, memfd_create};
use std::fs::File;
use std::io;
use std::os::unix::fs::FileExt;

const SHM_MAGIC: u32 = 0x5249_4E47; // "RING"
const HEADER_BYTES: usize = 64;

const OFF_MAGIC: usize = 0;
const OFF_ACTIVE_SLOT: usize = 8;
const OFF_GENERATION: usize = 16;
const OFF_WIDTH: usize = 24;
const OFF_HEIGHT: usize = 28;
const OFF_SLOT_CAPACITY: usize = 32;
const OFF_BYTE_LEN: usize = 40;
const OFF_SEQ: usize = 48;

fn read_u32_ne(buf: &[u8], off: usize) -> u32 {
    u32::from_ne_bytes([buf[off], buf[off + 1], buf[off + 2], buf[off + 3]])
}

fn read_u64_ne(buf: &[u8], off: usize) -> u64 {
    u64::from_ne_bytes([
        buf[off],
        buf[off + 1],
        buf[off + 2],
        buf[off + 3],
        buf[off + 4],
        buf[off + 5],
        buf[off + 6],
        buf[off + 7],
    ])
}

fn write_u32_ne(buf: &mut [u8], off: usize, val: u32) {
    buf[off..off + 4].copy_from_slice(&val.to_ne_bytes());
}

fn write_u64_ne(buf: &mut [u8], off: usize, val: u64) {
    buf[off..off + 8].copy_from_slice(&val.to_ne_bytes());
}

/// Ping-pong `memfd` shared memory writer owned by `fpshud-ui` (100% safe `FileExt` seqlock).
pub struct ShmRingWriter {
    file: File,
    total_len: usize,
    slot_capacity: usize,
    active_slot: u32,
    generation_seq: u64,
}

impl ShmRingWriter {
    pub fn new(width: u32, height: u32) -> io::Result<Self> {
        Self::allocate(width, height, 0)
    }

    pub fn validate_slot_capacity(width: u32, height: u32) -> io::Result<(usize, usize)> {
        let slot_capacity = usize::try_from(width.max(1))
            .ok()
            .and_then(|w| w.checked_mul(usize::try_from(height.max(1)).ok()?))
            .and_then(|px| px.checked_mul(4))
            .filter(|&cap| u32::try_from(cap).is_ok())
            .ok_or_else(|| io::Error::new(io::ErrorKind::InvalidInput, "SHM dimensions overflow"))?;
        let total_len = HEADER_BYTES
            .checked_add(slot_capacity.checked_mul(2).ok_or_else(|| {
                io::Error::new(io::ErrorKind::InvalidInput, "SHM ring overflow")
            })?)
            .ok_or_else(|| io::Error::new(io::ErrorKind::InvalidInput, "SHM total size overflow"))?;
        Ok((slot_capacity, total_len))
    }

    fn allocate(width: u32, height: u32, initial_gen: u64) -> io::Result<Self> {
        let (slot_capacity, total_len) = Self::validate_slot_capacity(width, height)?;

        let fd = memfd_create(c"fpshud_shm", MemfdFlags::CLOEXEC)?;
        ftruncate(&fd, total_len as u64)?;
        let file = File::from(fd);

        let mut hdr = [0_u8; HEADER_BYTES];
        write_u32_ne(&mut hdr, OFF_MAGIC, SHM_MAGIC);
        write_u32_ne(&mut hdr, OFF_ACTIVE_SLOT, 1);
        write_u64_ne(&mut hdr, OFF_GENERATION, initial_gen);
        write_u32_ne(&mut hdr, OFF_WIDTH, width);
        write_u32_ne(&mut hdr, OFF_HEIGHT, height);
        write_u64_ne(&mut hdr, OFF_SLOT_CAPACITY, slot_capacity as u64);
        write_u32_ne(&mut hdr, OFF_BYTE_LEN, 0);
        write_u64_ne(&mut hdr, OFF_SEQ, initial_gen.wrapping_mul(2));
        file.write_all_at(&hdr, 0)?;

        Ok(Self {
            file,
            total_len,
            slot_capacity,
            active_slot: 1,
            generation_seq: initial_gen,
        })
    }

    #[must_use]
    pub fn fd(&self) -> BorrowedFd<'_> {
        self.file.as_fd()
    }

    #[must_use]
    pub fn mapped_byte_len(&self) -> usize {
        self.total_len
    }

    pub fn resize(&mut self, width: u32, height: u32) -> io::Result<()> {
        *self = Self::allocate(width, height, self.generation_seq)?;
        Ok(())
    }

    pub fn write_next_slot(
        &mut self,
        width: u32,
        height: u32,
        rgba: &[u8],
    ) -> io::Result<ShmSlotMeta> {
        self.write_next_slot_at(0, 0, width, height, rgba)
    }

    pub fn write_next_slot_at(
        &mut self,
        dst_x: i32,
        dst_y: i32,
        width: u32,
        height: u32,
        rgba: &[u8],
    ) -> io::Result<ShmSlotMeta> {
        let byte_len_u32 = u32::try_from(rgba.len()).map_err(|_| {
            io::Error::new(io::ErrorKind::InvalidInput, "frame payload exceeds u32::MAX")
        })?;
        if rgba.len() > self.slot_capacity {
            return Err(io::Error::new(
                io::ErrorKind::InvalidInput,
                format!(
                    "frame {} bytes exceeds SHM slot capacity {}",
                    rgba.len(),
                    self.slot_capacity
                ),
            ));
        }

        let next_slot = self.active_slot ^ 1;
        if next_slot > 1 {
            return Err(io::Error::new(
                io::ErrorKind::InvalidData,
                "corrupt active SHM slot index",
            ));
        }

        let offset = HEADER_BYTES + (next_slot as usize) * self.slot_capacity;
        let end = offset
            .checked_add(rgba.len())
            .filter(|&e| e <= self.total_len)
            .ok_or_else(|| io::Error::new(io::ErrorKind::InvalidInput, "SHM slot bounds overflow"))?;
        debug_assert_eq!(end - offset, rgba.len());

        // Step 1: Mark seqlock odd (write in progress)
        let odd_seq = self.generation_seq.wrapping_mul(2).wrapping_add(1);
        self.file.write_all_at(&odd_seq.to_ne_bytes(), OFF_SEQ as u64)?;

        // Step 2: Write pixel payload into inactive slot
        self.file.write_all_at(rgba, offset as u64)?;

        // Step 3: Write updated header with seq still odd
        let generation = self.generation_seq + 1;
        let mut hdr = [0_u8; HEADER_BYTES];
        write_u32_ne(&mut hdr, OFF_MAGIC, SHM_MAGIC);
        write_u32_ne(&mut hdr, OFF_ACTIVE_SLOT, next_slot);
        write_u64_ne(&mut hdr, OFF_GENERATION, generation);
        write_u32_ne(&mut hdr, OFF_WIDTH, width);
        write_u32_ne(&mut hdr, OFF_HEIGHT, height);
        write_u64_ne(&mut hdr, OFF_SLOT_CAPACITY, self.slot_capacity as u64);
        write_u32_ne(&mut hdr, OFF_BYTE_LEN, byte_len_u32);
        write_u64_ne(&mut hdr, OFF_SEQ, odd_seq);
        self.file.write_all_at(&hdr, 0)?;

        // Step 4: Commit seqlock even
        let even_seq = generation.wrapping_mul(2);
        self.file.write_all_at(&even_seq.to_ne_bytes(), OFF_SEQ as u64)?;

        self.active_slot = next_slot;
        self.generation_seq = generation;

        Ok(ShmSlotMeta {
            modifier: 0,
            generation,
            slot_index: next_slot,
            dst_x,
            dst_y,
            width,
            height,
            stride: width.saturating_mul(4),
            fourcc: 0x3432_5241,
            byte_len: byte_len_u32,
            backend: FrameBackendKind::MemfdRgba,
        })
    }
}

/// Safe `memfd` seqlock consumer used inside `libfpshud.so` (zero `unsafe`).
pub struct ShmRingReader {
    file: File,
    total_len: usize,
}

impl ShmRingReader {
    pub fn from_borrowed_fd(fd: BorrowedFd<'_>, expected_len: usize) -> io::Result<Self> {
        let file = File::from(fd.try_clone_to_owned()?);
        let actual_len = usize::try_from(file.metadata()?.len())
            .map_err(|_| io::Error::new(io::ErrorKind::InvalidData, "invalid shm size"))?;
        let map_len = actual_len.max(expected_len);
        if map_len < HEADER_BYTES || map_len > actual_len {
            return Err(io::Error::new(
                io::ErrorKind::InvalidInput,
                "invalid SHM file backing size",
            ));
        }

        let mut magic_buf = [0_u8; 4];
        file.read_exact_at(&mut magic_buf, OFF_MAGIC as u64)?;
        if u32::from_ne_bytes(magic_buf) != SHM_MAGIC {
            return Err(io::Error::new(
                io::ErrorKind::InvalidData,
                "SHM magic mismatch",
            ));
        }

        Ok(Self {
            file,
            total_len: map_len,
        })
    }

    pub fn read_latest_into(&self, dst: &mut Vec<u8>) -> Option<(u64, u32)> {
        let mut hdr = [0_u8; HEADER_BYTES];
        self.file.read_exact_at(&mut hdr, 0).ok()?;

        if read_u32_ne(&hdr, OFF_MAGIC) != SHM_MAGIC {
            return None;
        }
        let seq_before = read_u64_ne(&hdr, OFF_SEQ);
        if (seq_before & 1) != 0 {
            return None;
        }
        let gen_val = read_u64_ne(&hdr, OFF_GENERATION);
        if gen_val == 0 {
            return None;
        }
        let slot = read_u32_ne(&hdr, OFF_ACTIVE_SLOT);
        let cap = usize::try_from(read_u64_ne(&hdr, OFF_SLOT_CAPACITY)).ok()?;
        let byte_len = read_u32_ne(&hdr, OFF_BYTE_LEN) as usize;
        if slot > 1 || byte_len > cap {
            return None;
        }

        let offset = HEADER_BYTES.checked_add((slot as usize).checked_mul(cap)?)?;
        let end = offset.checked_add(byte_len)?;
        if end > self.total_len {
            return None;
        }

        dst.resize(byte_len, 0);
        if self.file.read_exact_at(dst, offset as u64).is_err() {
            dst.clear();
            return None;
        }

        let mut gen_after_buf = [0_u8; 8];
        let mut seq_after_buf = [0_u8; 8];
        if self
            .file
            .read_exact_at(&mut gen_after_buf, OFF_GENERATION as u64)
            .is_err()
            || self
                .file
                .read_exact_at(&mut seq_after_buf, OFF_SEQ as u64)
                .is_err()
        {
            dst.clear();
            return None;
        }
        let gen_after = u64::from_ne_bytes(gen_after_buf);
        let seq_after = u64::from_ne_bytes(seq_after_buf);
        if gen_after != gen_val || seq_after != seq_before {
            dst.clear();
            return None;
        }

        Some((gen_val, slot))
    }

    #[must_use]
    pub fn read_latest(&self) -> Option<(u64, u32, Vec<u8>)> {
        let mut pixels = Vec::new();
        let (gen_val, slot) = self.read_latest_into(&mut pixels)?;
        Some((gen_val, slot, pixels))
    }
}
