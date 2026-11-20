use rustix::fd::{AsFd, BorrowedFd, OwnedFd};
use rustix::fs::{Mode, fcntl_getfl, fcntl_setfl, OFlags};
use rustix::net::{
    AddressFamily, RecvAncillaryBuffer, RecvAncillaryMessage, RecvFlags, SendAncillaryBuffer,
    SendAncillaryMessage, SendFlags, SocketAddrUnix, SocketFlags, SocketType, accept_with, bind,
    connect, listen, recvmsg, sendmsg, socket_with, sockopt::socket_peercred,
};
use rustix::process::getuid;
use std::fs;
use std::io::{self, IoSlice, IoSliceMut};
use std::os::unix::fs::PermissionsExt;
use std::path::{Path, PathBuf};

/// Verifies `SO_PEERCRED` peer UID equals the expected current user UID (`EACCES` otherwise).
pub fn verify_peer_credentials(peer_uid: u32, expected_uid: u32) -> io::Result<()> {
    if peer_uid == expected_uid {
        Ok(())
    } else {
        Err(io::Error::new(
            io::ErrorKind::PermissionDenied,
            format!("IPC peer uid mismatch: peer={peer_uid}, expected={expected_uid}"),
        ))
    }
}

pub struct IpcConnection {
    fd: OwnedFd,
}

impl IpcConnection {
    #[must_use]
    pub fn from_owned_fd(fd: OwnedFd) -> Self {
        Self { fd }
    }

    pub fn set_nonblocking(&self, nonblocking: bool) -> io::Result<()> {
        let mut flags = fcntl_getfl(&self.fd)?;
        if nonblocking {
            flags.insert(OFlags::NONBLOCK);
        } else {
            flags.remove(OFlags::NONBLOCK);
        }
        fcntl_setfl(&self.fd, flags)?;
        Ok(())
    }

    pub fn send_msg(&mut self, msg: &[u8], fds: &[BorrowedFd<'_>]) -> io::Result<()> {
        let iov = [IoSlice::new(msg)];
        let mut cmsg_space = [std::mem::MaybeUninit::uninit(); 64];
        let mut ancillary = SendAncillaryBuffer::new(&mut cmsg_space);
        if !fds.is_empty() && !ancillary.push(SendAncillaryMessage::ScmRights(fds)) {
            return Err(io::Error::new(
                io::ErrorKind::InvalidInput,
                "SCM_RIGHTS ancillary buffer overflow",
            ));
        }
        let sent = sendmsg(&self.fd, &iov, &mut ancillary, SendFlags::NOSIGNAL)?;
        if sent != msg.len() {
            return Err(io::Error::new(
                io::ErrorKind::WriteZero,
                "short SEQPACKET sendmsg",
            ));
        }
        Ok(())
    }

    pub fn recv_msg(&mut self, buf: &mut [u8]) -> io::Result<(usize, Vec<OwnedFd>)> {
        let mut iov = [IoSliceMut::new(buf)];
        let mut cmsg_space = [std::mem::MaybeUninit::uninit(); 64];
        let mut ancillary = RecvAncillaryBuffer::new(&mut cmsg_space);
        let res = recvmsg(&self.fd, &mut iov, &mut ancillary, RecvFlags::CMSG_CLOEXEC)?;
        let mut received_fds = Vec::new();
        for msg in ancillary.drain() {
            if let RecvAncillaryMessage::ScmRights(iter) = msg {
                received_fds.extend(iter);
            }
        }
        Ok((res.bytes, received_fds))
    }
}

pub struct IpcServer {
    fd: OwnedFd,
    sock_path: PathBuf,
}

impl IpcServer {
    pub fn bind(sock_path: &Path) -> io::Result<Self> {
        if let Some(parent) = sock_path.parent()
            && !parent.as_os_str().is_empty()
        {
            fs::create_dir_all(parent)?;
        }
        if let Ok(meta) = fs::symlink_metadata(sock_path) {
            if meta.is_dir() {
                return Err(io::Error::new(
                    io::ErrorKind::AlreadyExists,
                    "IPC socket path is a directory",
                ));
            }
            let _ = fs::remove_file(sock_path);
        }

        let fd = socket_with(
            AddressFamily::UNIX,
            SocketType::SEQPACKET,
            SocketFlags::CLOEXEC,
            None,
        )?;
        let addr = SocketAddrUnix::new(sock_path)?;
        bind(&fd, &addr)?;
        fs::set_permissions(sock_path, fs::Permissions::from_mode(Mode::RWXU.bits() & 0o600))?;
        listen(&fd, 4)?;
        Ok(Self {
            fd,
            sock_path: sock_path.to_path_buf(),
        })
    }

    pub fn set_nonblocking(&self, nonblocking: bool) -> io::Result<()> {
        let mut flags = fcntl_getfl(&self.fd)?;
        if nonblocking {
            flags.insert(OFlags::NONBLOCK);
        } else {
            flags.remove(OFlags::NONBLOCK);
        }
        fcntl_setfl(&self.fd, flags)?;
        Ok(())
    }

    pub fn accept_verified(&self) -> io::Result<IpcConnection> {
        let client_fd = accept_with(&self.fd, SocketFlags::CLOEXEC)?;
        let cred = socket_peercred(client_fd.as_fd())?;
        verify_peer_credentials(cred.uid.as_raw(), getuid().as_raw())?;
        Ok(IpcConnection::from_owned_fd(client_fd))
    }
}

impl Drop for IpcServer {
    fn drop(&mut self) {
        let _ = fs::remove_file(&self.sock_path);
    }
}

pub struct IpcClient;

impl IpcClient {
    pub fn connect(sock_path: &Path) -> io::Result<IpcConnection> {
        let fd = socket_with(
            AddressFamily::UNIX,
            SocketType::SEQPACKET,
            SocketFlags::CLOEXEC,
            None,
        )?;
        let addr = SocketAddrUnix::new(sock_path)?;
        connect(&fd, &addr)?;
        let cred = socket_peercred(fd.as_fd())?;
        verify_peer_credentials(cred.uid.as_raw(), getuid().as_raw())?;
        Ok(IpcConnection::from_owned_fd(fd))
    }
}
