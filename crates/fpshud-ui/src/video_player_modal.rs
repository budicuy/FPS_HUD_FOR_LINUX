use crate::cpu_canvas::{CpuCanvas, Rect, TextRasterizer};
use crate::state::{VideoPlayerUiState, rgba_u32};

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum PlayerHitAction {
    VideoArea,
    TogglePlay,
    SeekBack,
    SeekForward,
    ScrubBar,
    ToggleMute,
    ToggleFullscreen,
    OpenSystemPlayer,
    CloseModal,
}

#[derive(Debug, Clone, Copy, Default)]
pub struct VideoPlayerModalLayout {
    pub modal_rect: Rect,
    pub top_bar: Rect,
    pub video_rect: Rect,
    pub bottom_bar: Rect,
    pub scrub_rect: Rect,
    pub btn_play: Rect,
    pub btn_back: Rect,
    pub btn_fwd: Rect,
    pub btn_mute: Rect,
    pub btn_system: Rect,
    pub btn_full: Rect,
    pub btn_close: Rect,
}

/// Computes the centered aspect-fit rectangle inside `container` for a video of `(src_w, src_h)`.
/// Shared between `render_video_player_modal` (alpha punch-through) and `main.rs` (`wl_subsurface` geometry)
/// so both align to the exact pixel.
#[must_use]
pub fn compute_aspect_fit_rect(container: Rect, src_w: u32, src_h: u32) -> Rect {
    if src_w == 0 || src_h == 0 || container.w <= 1.0 || container.h <= 1.0 {
        return container;
    }
    let sw = src_w as f32;
    let sh = src_h as f32;
    let scale = (container.w / sw).min(container.h / sh);
    let fit_w = (sw * scale).clamp(1.0, container.w);
    let fit_h = (sh * scale).clamp(1.0, container.h);
    let off_x = container.x + (container.w - fit_w) * 0.5;
    let off_y = container.y + (container.h - fit_h) * 0.5;
    Rect::new(off_x, off_y, fit_w, fit_h)
}

#[must_use]
pub fn compute_modal_layout(vp_w: f32, vp_h: f32, fullscreen: bool) -> VideoPlayerModalLayout {
    let modal_rect = if fullscreen {
        Rect::new(0.0, 0.0, vp_w.max(320.0), vp_h.max(200.0))
    } else {
        let mw = (vp_w * 0.84)
            .clamp(520.0, 1080.0)
            .min((vp_w - 24.0).max(320.0));
        let mh = (vp_h * 0.84)
            .clamp(340.0, 720.0)
            .min((vp_h - 24.0).max(200.0));
        Rect::new(
            ((vp_w - mw) * 0.5).max(0.0),
            ((vp_h - mh) * 0.5).max(0.0),
            mw,
            mh,
        )
    };

    let top_h = if fullscreen { 40.0 } else { 34.0 };
    let bot_h = if fullscreen { 56.0 } else { 46.0 };
    let top_bar = Rect::new(modal_rect.x, modal_rect.y, modal_rect.w, top_h);
    let bottom_bar = Rect::new(
        modal_rect.x,
        (modal_rect.bottom() - bot_h).max(modal_rect.y + top_h),
        modal_rect.w,
        bot_h,
    );
    // Fullscreen: video fills 100% edge-to-edge (floating bars overlay on top without shrinking video).
    let video_rect = if fullscreen {
        modal_rect
    } else {
        Rect::new(
            modal_rect.x + 4.0,
            top_bar.bottom() + 2.0,
            (modal_rect.w - 8.0).max(64.0),
            (bottom_bar.y - top_bar.bottom() - 4.0).max(64.0),
        )
    };

    let top_btn_y = top_bar.y + (top_h - 24.0) * 0.5;
    let btn_close = Rect::new(top_bar.right() - 32.0, top_btn_y, 26.0, 24.0);
    let btn_full = Rect::new(btn_close.x - 56.0, top_btn_y, 50.0, 24.0);
    let btn_system = Rect::new(btn_full.x - 108.0, top_btn_y, 102.0, 24.0);

    let by = bottom_bar.y + if fullscreen { 14.0 } else { 10.0 };
    let btn_play = Rect::new(bottom_bar.x + 12.0, by, 56.0, 26.0);
    let btn_back = Rect::new(btn_play.right() + 6.0, by, 38.0, 26.0);
    let btn_fwd = Rect::new(btn_back.right() + 6.0, by, 38.0, 26.0);
    let btn_mute = Rect::new(bottom_bar.right() - 88.0, by, 76.0, 26.0);
    let scrub_x = btn_fwd.right() + 12.0;
    let scrub_w = (btn_mute.x - scrub_x - 12.0).max(40.0);
    let scrub_rect = Rect::new(scrub_x, by + 6.0, scrub_w, 14.0);

    VideoPlayerModalLayout {
        modal_rect,
        top_bar,
        video_rect,
        bottom_bar,
        scrub_rect,
        btn_play,
        btn_back,
        btn_fwd,
        btn_mute,
        btn_system,
        btn_full,
        btn_close,
    }
}

fn fmt_time(sec: f64) -> String {
    let total = sec.max(0.0).floor() as u64;
    format!("{:02}:{:02}", total / 60, total % 60)
}

pub fn render_video_player_modal(
    canvas: &mut CpuCanvas<'_>,
    rasterizer: &mut TextRasterizer,
    state: &VideoPlayerUiState,
    vp_w: f32,
    vp_h: f32,
    hovered: Option<PlayerHitAction>,
    out_hits: &mut Vec<(Rect, PlayerHitAction)>,
) -> VideoPlayerModalLayout {
    out_hits.clear();
    let layout = compute_modal_layout(vp_w, vp_h, state.fullscreen);
    let show_controls = !state.fullscreen || state.controls_visible;

    if !state.fullscreen {
        // Dim backdrop + modal frame in windowed mode
        canvas.fill_rect(Rect::new(0.0, 0.0, vp_w, vp_h), rgba_u32(6, 8, 12, 200));
        canvas.fill_rounded_rect(layout.modal_rect, 8.0, rgba_u32(14, 18, 26, 250));
        canvas.stroke_rounded_rect(layout.modal_rect, 8.0, 1.5, rgba_u32(82, 130, 180, 220));
    }

    // Video area: solid opaque black letterbox, then either alpha punch-through (Wayland GPU plane)
    // or CPU RGBA blit (X11 fallback).
    canvas.fill_rect(layout.video_rect, rgba_u32(0, 0, 0, 255));
    if state.external_video_plane {
        if state.video_src_w > 0 && state.video_src_h > 0 {
            let fitted =
                compute_aspect_fit_rect(layout.video_rect, state.video_src_w, state.video_src_h);
            canvas.clear_rect(fitted);
        } else {
            let status_txt = state.error.as_deref().unwrap_or("Loading video...");
            canvas.draw_text_in_rect(
                rasterizer,
                layout.video_rect,
                false,
                14.0,
                rgba_u32(180, 195, 215, 230),
                status_txt,
                true,
            );
        }
    } else if state.frame_w > 0 && state.frame_h > 0 && !state.frame_rgba.is_empty() {
        canvas.blit_rgba_aspect_fit(
            layout.video_rect,
            state.frame_w,
            state.frame_h,
            &state.frame_rgba,
        );
    } else {
        let status_txt = state.error.as_deref().unwrap_or("Loading video...");
        canvas.draw_text_in_rect(
            rasterizer,
            layout.video_rect,
            false,
            14.0,
            rgba_u32(180, 195, 215, 230),
            status_txt,
            true,
        );
    }

    // Push VideoArea FIRST so reverse hit-testing gives priority to buttons/bars drawn on top.
    out_hits.push((layout.video_rect, PlayerHitAction::VideoArea));

    if !show_controls {
        return layout;
    }

    let bar_bg = if state.fullscreen {
        rgba_u32(10, 14, 22, 205)
    } else {
        rgba_u32(20, 26, 38, 255)
    };

    // Top bar
    canvas.fill_rect(layout.top_bar, bar_bg);
    let title_w = (layout.btn_system.x - layout.top_bar.x - 18.0).max(60.0);
    canvas.draw_text_in_rect(
        rasterizer,
        Rect::new(
            layout.top_bar.x + 12.0,
            layout.top_bar.y + (layout.top_bar.h - 26.0) * 0.5,
            title_w,
            26.0,
        ),
        false,
        13.0,
        rgba_u32(235, 242, 252, 255),
        &state.title,
        false,
    );

    let draw_btn = |canvas: &mut CpuCanvas<'_>,
                    rasterizer: &mut TextRasterizer,
                    rect: Rect,
                    label: &str,
                    action: PlayerHitAction,
                    enabled: bool| {
        let is_hov = enabled && hovered == Some(action);
        let bg = if !enabled {
            rgba_u32(30, 34, 42, 180)
        } else if is_hov {
            rgba_u32(62, 118, 175, 255)
        } else {
            rgba_u32(36, 46, 64, 240)
        };
        let fg = if enabled {
            rgba_u32(240, 245, 255, 255)
        } else {
            rgba_u32(120, 130, 145, 200)
        };
        canvas.fill_rounded_rect(rect, 4.0, bg);
        canvas.draw_text_in_rect(rasterizer, rect, false, 11.0, fg, label, true);
    };

    draw_btn(
        canvas,
        rasterizer,
        layout.btn_system,
        "System Player",
        PlayerHitAction::OpenSystemPlayer,
        true,
    );
    draw_btn(
        canvas,
        rasterizer,
        layout.btn_full,
        if state.fullscreen { "Exit" } else { "Full" },
        PlayerHitAction::ToggleFullscreen,
        true,
    );
    draw_btn(
        canvas,
        rasterizer,
        layout.btn_close,
        "X",
        PlayerHitAction::CloseModal,
        true,
    );

    out_hits.push((layout.btn_system, PlayerHitAction::OpenSystemPlayer));
    out_hits.push((layout.btn_full, PlayerHitAction::ToggleFullscreen));
    out_hits.push((layout.btn_close, PlayerHitAction::CloseModal));

    // Bottom bar
    canvas.fill_rect(layout.bottom_bar, bar_bg);
    draw_btn(
        canvas,
        rasterizer,
        layout.btn_play,
        if state.playing { "Pause" } else { "Play" },
        PlayerHitAction::TogglePlay,
        true,
    );
    draw_btn(
        canvas,
        rasterizer,
        layout.btn_back,
        "-5s",
        PlayerHitAction::SeekBack,
        true,
    );
    draw_btn(
        canvas,
        rasterizer,
        layout.btn_fwd,
        "+5s",
        PlayerHitAction::SeekForward,
        true,
    );
    let mute_label = if !state.has_audio {
        "No Audio"
    } else if state.muted {
        "Muted"
    } else {
        "Audio"
    };
    draw_btn(
        canvas,
        rasterizer,
        layout.btn_mute,
        mute_label,
        PlayerHitAction::ToggleMute,
        state.has_audio,
    );

    out_hits.push((layout.btn_play, PlayerHitAction::TogglePlay));
    out_hits.push((layout.btn_back, PlayerHitAction::SeekBack));
    out_hits.push((layout.btn_fwd, PlayerHitAction::SeekForward));
    out_hits.push((layout.scrub_rect, PlayerHitAction::ScrubBar));
    if state.has_audio {
        out_hits.push((layout.btn_mute, PlayerHitAction::ToggleMute));
    }

    // Scrub bar & time label
    let disp_sec = state.scrub_preview_sec.unwrap_or(state.current_sec);
    let frac = if state.duration_sec > 0.001 {
        (disp_sec / state.duration_sec).clamp(0.0, 1.0) as f32
    } else {
        0.0
    };
    canvas.fill_rounded_rect(layout.scrub_rect, 4.0, rgba_u32(42, 52, 70, 240));
    let fill_w = (layout.scrub_rect.w * frac).max(0.0);
    if fill_w > 1.0 {
        canvas.fill_rounded_rect(
            Rect::new(
                layout.scrub_rect.x,
                layout.scrub_rect.y,
                fill_w,
                layout.scrub_rect.h,
            ),
            4.0,
            rgba_u32(95, 205, 255, 255),
        );
    }
    let time_str = format!("{} / {}", fmt_time(disp_sec), fmt_time(state.duration_sec));
    canvas.draw_text_in_rect(
        rasterizer,
        Rect::new(
            layout.scrub_rect.x,
            layout.bottom_bar.bottom() - 18.0,
            layout.scrub_rect.w,
            16.0,
        ),
        true,
        10.0,
        rgba_u32(190, 208, 230, 230),
        &time_str,
        true,
    );

    layout
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_fullscreen_layout_edge_to_edge_and_ultrawide_alpha_punch_through() {
        let vp_w = 2560u32;
        let vp_h = 1080u32;
        let mut buf = vec![0u8; (vp_w * vp_h * 4) as usize];
        let mut rast = TextRasterizer::new();
        let mut hits = Vec::new();
        let state = VideoPlayerUiState {
            open: true,
            title: "clip.mp4".to_string(),
            playing: true,
            fullscreen: true,
            controls_visible: false,
            external_video_plane: true,
            video_src_w: 1920,
            video_src_h: 1080,
            duration_sec: 10.0,
            current_sec: 2.0,
            ..VideoPlayerUiState::default()
        };
        {
            let mut canvas = CpuCanvas::new(&mut buf, vp_w, vp_h, 1.0);
            let layout = render_video_player_modal(
                &mut canvas,
                &mut rast,
                &state,
                vp_w as f32,
                vp_h as f32,
                None,
                &mut hits,
            );
            assert_eq!(layout.video_rect, Rect::new(0.0, 0.0, 2560.0, 1080.0));
            let fitted = compute_aspect_fit_rect(layout.video_rect, 1920, 1080);
            assert!((fitted.x - 320.0).abs() < 0.5 && (fitted.w - 1920.0).abs() < 0.5);
        }
        // Only VideoArea hit when controls are hidden
        assert_eq!(hits.len(), 1);
        assert_eq!(hits[0].1, PlayerHitAction::VideoArea);

        // Center of video is transparent punch-through [0, 0, 0, 0]
        let center_off = ((540 * vp_w + 1280) * 4) as usize;
        assert_eq!(&buf[center_off..center_off + 4], &[0, 0, 0, 0]);
        // Left & right letterbox are solid opaque black [0, 0, 0, 255]
        let left_off = ((540 * vp_w + 100) * 4) as usize;
        let right_off = ((540 * vp_w + 2450) * 4) as usize;
        assert_eq!(&buf[left_off..left_off + 4], &[0, 0, 0, 255]);
        assert_eq!(&buf[right_off..right_off + 4], &[0, 0, 0, 255]);

        // With controls visible, floating bars have alpha > 0 and buttons are hittable
        let mut state_vis = state;
        state_vis.controls_visible = true;
        {
            let mut canvas = CpuCanvas::new(&mut buf, vp_w, vp_h, 1.0);
            let _ = render_video_player_modal(
                &mut canvas,
                &mut rast,
                &state_vis,
                vp_w as f32,
                vp_h as f32,
                None,
                &mut hits,
            );
        }
        assert!(hits.iter().any(|(_, a)| *a == PlayerHitAction::TogglePlay));
        assert!(hits.iter().any(|(_, a)| *a == PlayerHitAction::ToggleFullscreen));
        let bot_off = (((vp_h - 10) * vp_w + 1280) * 4) as usize;
        assert!(buf[bot_off + 3] > 0, "floating bottom bar must be visible");
    }
}
