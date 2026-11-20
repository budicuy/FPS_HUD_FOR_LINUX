use crate::protocol::{FrameBackendKind, ShmSlotMeta};
use memmap2::{MmapOptions, MmapRaw};
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

unsafe fn aligned_raw_ptr<T>(base: *const u8, mapped_len: usize, offset: usize) -> *const T {
    assert!(!base.is_null(), "SHM mapping pointer is null");
    assert!(mapped_len >= HEADER_BYTES, "SHM buffer shorter than header");
    assert!(
        offset + size_of::<T>() <= HEADER_BYTES,
        "SHM header field offset out of bounds"
    );
    // SAFETY: `offset + size_of::<T>() <= HEADER_BYTES <= mapped_len`, so pointer stays inside `MmapRaw`.
    let ptr = unsafe { base.add(offset).cast::<T>() };
    assert!(ptr.is_aligned(), "SHM atomic field pointer misaligned");
    ptr
}

unsafe fn raw_atomic_u32<'a>(base: *const u8, mapped_len: usize, offset: usize) -> &'a AtomicU32 {
    let ptr = unsafe { aligned_raw_ptr::<AtomicU32>(base, mapped_len, offset) };
    // SAFETY: `ptr` comes directly from `MmapRaw` (`as_ptr`/`as_mut_ptr`), avoiding `&[u8]` freeze provenance.
    unsafe { &*ptr }
}

unsafe fn raw_atomic_u64<'a>(base: *const u8, mapped_len: usize, offset: usize) -> &'a AtomicU64 {
    let ptr = unsafe { aligned_raw_ptr::<AtomicU64>(base, mapped_len, offset) };
    // SAFETY: `ptr` comes directly from `MmapRaw` (`as_ptr`/`as_mut_ptr`), avoiding `&[u8]` freeze provenance.
    unsafe { &*ptr }
}

/// Ping-pong `memfd` shared memory writer owned by `fpshud-ui`.
pub struct ShmRingWriter {
    file: File,
    mmap: MmapRaw,
    slot_capacity: usize,
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
        // `file` is an anonymous kernel `memfd` sized to `total_len` bytes above; `MmapRaw` keeps header atomics free of `&[u8]` freeze borrows.
        let mmap = MmapOptions::new().len(total_len).map_raw(&file)?;
        let base = mmap.as_mut_ptr().cast_const();
        let mapped_len = mmap.len();

        unsafe {
            raw_atomic_u32(base, mapped_len, OFF_ACTIVE_SLOT).store(1, Ordering::Relaxed);
            raw_atomic_u64(base, mapped_len, OFF_GENERATION).store(initial_gen, Ordering::Relaxed);
            raw_atomic_u32(base, mapped_len, OFF_WIDTH).store(width, Ordering::Relaxed);
            raw_atomic_u32(base, mapped_len, OFF_HEIGHT).store(height, Ordering::Relaxed);
            raw_atomic_u64(base, mapped_len, OFF_SLOT_CAPACITY)
                .store(slot_capacity as u64, Ordering::Relaxed);
            raw_atomic_u32(base, mapped_len, OFF_BYTE_LEN).store(0, Ordering::Relaxed);
            raw_atomic_u32(base, mapped_len, OFF_MAGIC).store(SHM_MAGIC, Ordering::Release);
        }

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

        let mut_base = self.mmap.as_mut_ptr();
        let base = mut_base.cast_const();
        let mapped_len = self.mmap.len();
        let prev_slot = unsafe { raw_atomic_u32(base, mapped_len, OFF_ACTIVE_SLOT) }
            .load(Ordering::Relaxed);
        let next_slot = prev_slot ^ 1;
        if next_slot > 1 {
            return Err(io::Error::new(
                io::ErrorKind::InvalidData,
                "corrupt active SHM slot index",
            ));
        }

        let offset = HEADER_BYTES + (next_slot as usize) * self.slot_capacity;
        let end = offset
            .checked_add(rgba.len())
            .filter(|&e| e <= mapped_len)
            .ok_or_else(|| io::Error::new(io::ErrorKind::InvalidInput, "SHM slot bounds overflow"))?;
        debug_assert_eq!(end - offset, rgba.len());

        // SAFETY: `offset + rgba.len() <= mapped_len`, destination is the inactive slot inside `MmapRaw`, disjoint from `rgba`.
        unsafe {
            std::ptr::copy_nonoverlapping(rgba.as_ptr(), mut_base.add(offset), rgba.len());
        }

        self.generation_seq += 1;
        let generation = self.generation_seq;
        unsafe {
            raw_atomic_u32(base, mapped_len, OFF_WIDTH).store(width, Ordering::Release);
            raw_atomic_u32(base, mapped_len, OFF_HEIGHT).store(height, Ordering::Release);
            raw_atomic_u32(base, mapped_len, OFF_BYTE_LEN).store(byte_len_u32, Ordering::Release);
            raw_atomic_u32(base, mapped_len, OFF_ACTIVE_SLOT).store(next_slot, Ordering::Release);
            raw_atomic_u64(base, mapped_len, OFF_GENERATION).store(generation, Ordering::Release);
        }

        Ok(ShmSlotMeta {
            modifier: 0,
            generation,
            slot_index: next_slot,
            width,
            height,
            stride: width.saturating_mul(4),
            fourcc: 0x3432_5241,
            byte_len: byte_len_u32,
            backend: FrameBackendKind::MemfdRgba,
        })
    }
}

/// Lock-free read-only `memfd` consumer mapped inside `libfpshud.so`.
pub struct ShmRingReader {
    mmap: MmapRaw,
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
        // `map_len` is bounded by `actual_len` above; `map_raw_read_only` avoids aliasing `&[u8]` over shared atomic header.
        let mmap = MmapOptions::new().len(map_len).map_raw_read_only(&file)?;
        if unsafe { raw_atomic_u32(mmap.as_ptr(), mmap.len(), OFF_MAGIC) }.load(Ordering::Acquire)
            != SHM_MAGIC
        {
            return Err(io::Error::new(
                io::ErrorKind::InvalidData,
                "SHM magic mismatch",
            ));
        }
        Ok(Self { mmap })
    }

    #[must_use]
    pub fn read_latest(&self) -> Option<(u64, u32, &[u8])> {
        let base = self.mmap.as_ptr();
        let len = self.mmap.len();
        let gen_val = unsafe { raw_atomic_u64(base, len, OFF_GENERATION) }.load(Ordering::Acquire);
        if gen_val == 0 {
            return None;
        }
        let slot = unsafe { raw_atomic_u32(base, len, OFF_ACTIVE_SLOT) }.load(Ordering::Acquire);
        let cap = usize::try_from(
            unsafe { raw_atomic_u64(base, len, OFF_SLOT_CAPACITY) }.load(Ordering::Acquire),
        )
        .ok()?;
        let byte_len =
            unsafe { raw_atomic_u32(base, len, OFF_BYTE_LEN) }.load(Ordering::Acquire) as usize;
        // Re-verify generation after metadata loads to guard against torn slot/generation pairs.
        let gen_after =
            unsafe { raw_atomic_u64(base, len, OFF_GENERATION) }.load(Ordering::Acquire);
        if gen_after != gen_val || slot > 1 || byte_len > cap {
            return None;
        }
        let offset = HEADER_BYTES.checked_add((slot as usize).checked_mul(cap)?)?;
        let end = offset.checked_add(byte_len)?;
        if end > len {
            return None;
        }
        // SAFETY: `offset..end <= self.mmap.len()`, `base` is valid and non-null, and `u8` has alignment 1.
        let pixels = unsafe { std::slice::from_raw_parts(base.add(offset), byte_len) };
        Some((gen_val, slot, pixels))
    }
}
