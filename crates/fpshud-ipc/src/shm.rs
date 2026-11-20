use crate::protocol::{FrameBackendKind, ShmSlotMeta};
use memmap2::{Mmap, MmapMut, MmapOptions};
use rustix::fd::{AsFd, BorrowedFd};
use rustix::fs::{MemfdFlags, ftruncate, memfd_create};
use std::fs::File;
use std::io;
use std::sync::atomic::{AtomicU32, AtomicU64, Ordering};

const SHM_MAGIC: u32 = 0x5249_4E47; // "RING"
const HEADER_BYTES: usize = 64;

const OFF_MAGIC: usize = 0;
const OFF_ACTIVE_SLOT: usize = 8;
const OFF_GENERATION: usize = 16;
const OFF_WIDTH: usize = 24;
const OFF_HEIGHT: usize = 28;
const OFF_SLOT_CAPACITY: usize = 32;
const OFF_BYTE_LEN: usize = 40;

fn aligned_header_ptr<T>(buf: &[u8], offset: usize) -> *const T {
    assert!(buf.len() >= HEADER_BYTES, "SHM buffer shorter than header");
    assert!(
        offset + size_of::<T>() <= HEADER_BYTES,
        "SHM header field offset out of bounds"
    );
    // SAFETY: `offset + size_of::<T>() <= HEADER_BYTES <= buf.len()`, so the pointer lies inside `buf`.
    let ptr = unsafe { buf.as_ptr().add(offset).cast::<T>() };
    assert!(ptr.is_aligned(), "SHM atomic field pointer misaligned");
    ptr
}

fn atomic_u32(buf: &[u8], offset: usize) -> &AtomicU32 {
    let ptr = aligned_header_ptr::<AtomicU32>(buf, offset);
    // SAFETY: `ptr` is non-null, properly aligned, and points into shared `memfd` pages with atomic access.
    unsafe { &*ptr }
}

fn atomic_u64(buf: &[u8], offset: usize) -> &AtomicU64 {
    let ptr = aligned_header_ptr::<AtomicU64>(buf, offset);
    // SAFETY: `ptr` is non-null, properly aligned, and points into shared `memfd` pages with atomic access.
    unsafe { &*ptr }
}

/// Ping-pong `memfd` shared memory writer owned by `fpshud-ui`.
pub struct ShmRingWriter {
    file: File,
    mmap: MmapMut,
    slot_capacity: usize,
    generation_seq: u64,
}

impl ShmRingWriter {
    pub fn new(width: u32, height: u32) -> io::Result<Self> {
        Self::allocate(width, height, 0)
    }

    fn allocate(width: u32, height: u32, initial_gen: u64) -> io::Result<Self> {
        let slot_capacity = usize::try_from(width.max(1))
            .ok()
            .and_then(|w| w.checked_mul(usize::try_from(height.max(1)).ok()?))
            .and_then(|px| px.checked_mul(4))
            .ok_or_else(|| io::Error::new(io::ErrorKind::InvalidInput, "SHM dimensions overflow"))?;
        let total_len = HEADER_BYTES
            .checked_add(slot_capacity.checked_mul(2).ok_or_else(|| {
                io::Error::new(io::ErrorKind::InvalidInput, "SHM ring overflow")
            })?)
            .ok_or_else(|| io::Error::new(io::ErrorKind::InvalidInput, "SHM total size overflow"))?;

        let fd = memfd_create(c"fpshud_shm", MemfdFlags::CLOEXEC)?;
        ftruncate(&fd, total_len as u64)?;
        let file = File::from(fd);
        // SAFETY: `file` is an anonymous kernel `memfd` sized to `total_len` bytes cleanly above.
        let mmap = unsafe { MmapOptions::new().len(total_len).map_mut(&file)? };

        atomic_u32(&mmap, OFF_ACTIVE_SLOT).store(1, Ordering::Relaxed);
        atomic_u64(&mmap, OFF_GENERATION).store(initial_gen, Ordering::Relaxed);
        atomic_u32(&mmap, OFF_WIDTH).store(width, Ordering::Relaxed);
        atomic_u32(&mmap, OFF_HEIGHT).store(height, Ordering::Relaxed);
        atomic_u64(&mmap, OFF_SLOT_CAPACITY).store(slot_capacity as u64, Ordering::Relaxed);
        atomic_u32(&mmap, OFF_BYTE_LEN).store(0, Ordering::Relaxed);
        atomic_u32(&mmap, OFF_MAGIC).store(SHM_MAGIC, Ordering::Release);

        Ok(Self {
            file,
            mmap,
            slot_capacity,
            generation_seq: initial_gen,
        })
    }

    #[must_use]
    pub fn fd(&self) -> BorrowedFd<'_> {
        self.file.as_fd()
    }

    #[must_use]
    pub fn mapped_byte_len(&self) -> usize {
        self.mmap.len()
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

        let prev_slot = atomic_u32(&self.mmap, OFF_ACTIVE_SLOT).load(Ordering::Relaxed);
        let next_slot = prev_slot ^ 1;
        let offset = HEADER_BYTES + (next_slot as usize) * self.slot_capacity;
        self.mmap[offset..offset + rgba.len()].copy_from_slice(rgba);

        self.generation_seq += 1;
        let generation = self.generation_seq;
        atomic_u32(&self.mmap, OFF_WIDTH).store(width, Ordering::Release);
        atomic_u32(&self.mmap, OFF_HEIGHT).store(height, Ordering::Release);
        atomic_u32(&self.mmap, OFF_BYTE_LEN).store(rgba.len() as u32, Ordering::Release);
        atomic_u32(&self.mmap, OFF_ACTIVE_SLOT).store(next_slot, Ordering::Release);
        atomic_u64(&self.mmap, OFF_GENERATION).store(generation, Ordering::Release);

        Ok(ShmSlotMeta {
            modifier: 0,
            generation,
            slot_index: next_slot,
            width,
            height,
            stride: width.saturating_mul(4),
            fourcc: 0x3432_5241,
            byte_len: rgba.len() as u32,
            backend: FrameBackendKind::MemfdRgba,
        })
    }
}

/// Lock-free read-only `memfd` consumer mapped inside `libfpshud.so`.
pub struct ShmRingReader {
    mmap: Mmap,
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
        // SAFETY: `map_len` is bounded by the underlying `memfd` size `actual_len` above.
        let mmap = unsafe { MmapOptions::new().len(map_len).map(&file)? };
        if atomic_u32(&mmap, OFF_MAGIC).load(Ordering::Acquire) != SHM_MAGIC {
            return Err(io::Error::new(
                io::ErrorKind::InvalidData,
                "SHM magic mismatch",
            ));
        }
        Ok(Self { mmap })
    }

    #[must_use]
    pub fn read_latest(&self) -> Option<(u64, u32, &[u8])> {
        let gen_val = atomic_u64(&self.mmap, OFF_GENERATION).load(Ordering::Acquire);
        if gen_val == 0 {
            return None;
        }
        let slot = atomic_u32(&self.mmap, OFF_ACTIVE_SLOT).load(Ordering::Acquire);
        let cap = usize::try_from(atomic_u64(&self.mmap, OFF_SLOT_CAPACITY).load(Ordering::Acquire))
            .ok()?;
        let byte_len = atomic_u32(&self.mmap, OFF_BYTE_LEN).load(Ordering::Acquire) as usize;
        if slot > 1 || byte_len > cap {
            return None;
        }
        let offset = HEADER_BYTES.checked_add((slot as usize).checked_mul(cap)?)?;
        let end = offset.checked_add(byte_len)?;
        if end > self.mmap.len() {
            return None;
        }
        Some((gen_val, slot, &self.mmap[offset..end]))
    }
}
