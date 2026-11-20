use crate::protocol::{FrameBackendKind, ShmSlotMeta};
use rustix::fd::{AsFd, BorrowedFd, OwnedFd};
use rustix::fs::{MemfdFlags, fstat, ftruncate, memfd_create};
use rustix::mm::{MapFlags, ProtFlags, mmap, munmap};
use std::ffi::c_void;
use std::io;
use std::ptr::NonNull;
use std::sync::atomic::{AtomicU32, AtomicU64, Ordering};

const SHM_MAGIC: u32 = 0x5249_4E47; // "RING"
const HEADER_BYTES: usize = 64;

#[repr(C)]
struct ShmHeader {
    magic: u32,
    active_slot: AtomicU32,
    generation: AtomicU64,
    width: AtomicU32,
    height: AtomicU32,
    slot_capacity: u64,
    byte_len: AtomicU32,
    _pad: [u8; 28],
}

const _: () = assert!(size_of::<ShmHeader>() == HEADER_BYTES);

struct ShmMapping {
    ptr: NonNull<c_void>,
    len: usize,
}

unsafe impl Send for ShmMapping {}
unsafe impl Sync for ShmMapping {}

impl Drop for ShmMapping {
    fn drop(&mut self) {
        unsafe {
            let _ = munmap(self.ptr.as_ptr(), self.len);
        }
    }
}

/// Ping-pong `memfd` shared memory writer owned by `fpshud-ui`.
pub struct ShmRingWriter {
    fd: OwnedFd,
    mapping: ShmMapping,
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
        let raw_ptr = unsafe {
            mmap(
                std::ptr::null_mut(),
                total_len,
                ProtFlags::READ | ProtFlags::WRITE,
                MapFlags::SHARED,
                &fd,
                0,
            )?
        };
        let ptr = NonNull::new(raw_ptr)
            .ok_or_else(|| io::Error::other("mmap returned null pointer"))?;
        let mapping = ShmMapping {
            ptr,
            len: total_len,
        };

        unsafe {
            let hdr = mapping.ptr.as_ptr().cast::<ShmHeader>();
            (*hdr).magic = SHM_MAGIC;
            (*hdr).active_slot = AtomicU32::new(1); // Next slot wraps cleanly to 0 first
            (*hdr).generation = AtomicU64::new(initial_gen);
            (*hdr).width = AtomicU32::new(width);
            (*hdr).height = AtomicU32::new(height);
            (*hdr).slot_capacity = slot_capacity as u64;
            (*hdr).byte_len = AtomicU32::new(0);
        }

        Ok(Self {
            fd,
            mapping,
            slot_capacity,
            generation_seq: initial_gen,
        })
    }

    #[must_use]
    pub fn fd(&self) -> BorrowedFd<'_> {
        self.fd.as_fd()
    }

    #[must_use]
    pub fn mapped_byte_len(&self) -> usize {
        self.mapping.len
    }

    pub fn resize(&mut self, width: u32, height: u32) -> io::Result<()> {
        let next = Self::allocate(width, height, self.generation_seq)?;
        *self = next;
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

        let hdr = unsafe { &*self.mapping.ptr.as_ptr().cast::<ShmHeader>() };
        let prev_slot = hdr.active_slot.load(Ordering::Relaxed);
        let next_slot = prev_slot ^ 1;
        let offset = HEADER_BYTES + (next_slot as usize) * self.slot_capacity;

        unsafe {
            let dst = self.mapping.ptr.as_ptr().cast::<u8>().add(offset);
            std::ptr::copy_nonoverlapping(rgba.as_ptr(), dst, rgba.len());
        }

        self.generation_seq += 1;
        let generation = self.generation_seq;
        hdr.width.store(width, Ordering::Release);
        hdr.height.store(height, Ordering::Release);
        hdr.byte_len.store(rgba.len() as u32, Ordering::Release);
        hdr.active_slot.store(next_slot, Ordering::Release);
        hdr.generation.store(generation, Ordering::Release);

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
    mapping: ShmMapping,
}

impl ShmRingReader {
    pub fn from_borrowed_fd(fd: BorrowedFd<'_>, expected_len: usize) -> io::Result<Self> {
        let st = fstat(fd)?;
        let actual_len = usize::try_from(st.st_size)
            .map_err(|_| io::Error::new(io::ErrorKind::InvalidData, "invalid shm size"))?;
        let map_len = actual_len.max(expected_len);
        if map_len < HEADER_BYTES || map_len > actual_len {
            return Err(io::Error::new(
                io::ErrorKind::InvalidInput,
                "invalid SHM file backing size",
            ));
        }
        let raw_ptr = unsafe {
            mmap(
                std::ptr::null_mut(),
                map_len,
                ProtFlags::READ,
                MapFlags::SHARED,
                fd,
                0,
            )?
        };
        let ptr = NonNull::new(raw_ptr)
            .ok_or_else(|| io::Error::other("mmap returned null pointer"))?;
        let mapping = ShmMapping { ptr, len: map_len };
        let hdr = unsafe { &*mapping.ptr.as_ptr().cast::<ShmHeader>() };
        if hdr.magic != SHM_MAGIC {
            return Err(io::Error::new(
                io::ErrorKind::InvalidData,
                "SHM magic mismatch",
            ));
        }
        Ok(Self { mapping })
    }

    #[must_use]
    pub fn read_latest(&self) -> Option<(u64, u32, &[u8])> {
        let hdr = unsafe { &*self.mapping.ptr.as_ptr().cast::<ShmHeader>() };
        let gen_val = hdr.generation.load(Ordering::Acquire);
        if gen_val == 0 {
            return None;
        }
        let slot = hdr.active_slot.load(Ordering::Acquire);
        let cap = hdr.slot_capacity as usize;
        let byte_len = (hdr.byte_len.load(Ordering::Acquire) as usize).min(cap);
        let offset = HEADER_BYTES.checked_add((slot as usize).checked_mul(cap)?)?;
        let end = offset.checked_add(byte_len)?;
        if slot > 1 || end > self.mapping.len {
            return None;
        }
        let slice = unsafe {
            let ptr = self.mapping.ptr.as_ptr().cast::<u8>().add(offset);
            std::slice::from_raw_parts(ptr, byte_len)
        };
        Some((gen_val, slot, slice))
    }
}
