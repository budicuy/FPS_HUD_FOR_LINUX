use gpui::PlatformTextSystem;
use gpui_ce_parley::{FontDataBlob, ParleyTextSystem, SystemFonts};
use std::borrow::Cow;
use std::sync::Arc;

pub const OPEN_SANS_BOLD_TTF: &[u8] =
    include_bytes!("../../../assets/fonts/OpenSans-Bold.ttf");
pub const VERA_MONO_BOLD_TTF: &[u8] =
    include_bytes!("../../../assets/fonts/VeraMono-Bold.ttf");

/// Validates that embedded TTF font blobs carry recognized SFNT / OpenType headers.
#[must_use]
pub fn validate_embedded_font_headers() -> bool {
    let valid_sfnt = |bytes: &[u8]| -> bool {
        if bytes.len() < 12 {
            return false;
        }
        matches!(
            &bytes[0..4],
            [0x00, 0x01, 0x00, 0x00] | b"OTTO" | b"true" | b"ttcf"
        )
    };
    valid_sfnt(OPEN_SANS_BOLD_TTF) && valid_sfnt(VERA_MONO_BOLD_TTF)
}

/// Registers both embedded TTF fonts onto any GPUI `PlatformTextSystem`.
pub fn register_embedded_fonts(text_system: &dyn PlatformTextSystem) -> anyhow::Result<()> {
    let _probe_sans = FontDataBlob::from(OPEN_SANS_BOLD_TTF.to_vec());
    let _probe_mono = FontDataBlob::from(VERA_MONO_BOLD_TTF.to_vec());
    text_system.add_fonts(vec![
        Cow::Borrowed(OPEN_SANS_BOLD_TTF),
        Cow::Borrowed(VERA_MONO_BOLD_TTF),
    ])
}

/// Creates a `ParleyTextSystem` with OS/fontconfig discovery disabled (`SystemFonts::Skip`).
#[must_use]
pub fn create_embedded_text_system() -> Arc<dyn PlatformTextSystem> {
    let ts = ParleyTextSystem::new_with_system_font(SystemFonts::Skip, "Open Sans");
    let _ = register_embedded_fonts(&ts);
    Arc::new(ts)
}
