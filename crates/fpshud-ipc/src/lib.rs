#![forbid(unsafe_code)]
//! Fixed-width zero-`usize` IPC framing, `SO_PEERCRED` Unix sockets, and `memfd`/DMA-BUF ring.

pub mod protocol;
pub mod shm;
pub mod socket;

pub use protocol::{FrameBackendKind, LayerToUiMsg, ShmSlotMeta, UiToLayerMsg};
pub use shm::{ShmRingReader, ShmRingWriter};
pub use socket::{IpcClient, IpcConnection, IpcServer};
