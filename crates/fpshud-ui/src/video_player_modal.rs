use crate::cpu_canvas::{CpuCanvas, Rect, TextRasterizer};
use crate::state::{VideoPlayerUiState, rgba_u32};

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum PlayerHitAction {
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

#[must_use]
pub fn compute_modal_layout(
    vp_w: f32,
    vp_h: f32,
    fullscreen: bool,
) -> VideoPlayerModalLayout {
    let modal_rect = if fullscreen {
        Rect::new(0.0, 0.0, vp_w.max(320.0), vp_h.max(200.0))
    } else {
        let mw = (vp_w * 0.84).clamp(520.0, 1080.0).min((vp_w - 24.0).max(320.0));
        let mh = (vp_h * 0.84).clamp(340.0, 720.0).min((vp_h - 24.0).max(200.0));
        Rect::new(((vp_w - mw) * 0.5).max(0.0), ((vp_h - mh) * 0.5).max(0.0), mw, mh)
    };

    let top_h = 34.0;
    let bot_h = 46.0;
    let top_bar = Rect::new(modal_rect.x, modal_rect.y, modal_rect.w, top_h);
    let bottom_bar = Rect::new(
        modal_rect.x,
        (modal_rect.bottom() - bot_h).max(modal_rect.y + top_h),
        modal_rect.w,
        bot_h,
    );
    let video_rect = Rect::new(
        modal_rect.x + 4.0,
        top_bar.bottom() + 2.0,
        (modal_rect.w - 8.0).max(64.0),
        (bottom_bar.y - top_bar.bottom() - 4.0).max(64.0),
    );

    let btn_close = Rect::new(top_bar.right() - 32.0, top_bar.y + 5.0, 26.0, 24.0);
    let btn_full = Rect::new(btn_close.x - 56.0, top_bar.y + 5.0, 50.0, 24.0);
    let btn_system = Rect::new(btn_full.x - 108.0, top_bar.y + 5.0, 102.0, 24.0);

    let by = bottom_bar.y + 10.0;
    let btn_play = Rect::new(bottom_bar.x + 10.0, by, 56.0, 26.0);
    let btn_back = Rect::new(btn_play.right() + 6.0, by, 38.0, 26.0);
    let btn_fwd = Rect::new(btn_back.right() + 6.0, by, 38.0, 26.0);
    let btn_mute = Rect::new(bottom_bar.right() - 86.0, by, 76.0, 26.0);
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

    // Dim backdrop
    canvas.fill_rect(Rect::new(0.0, 0.0, vp_w, vp_h), rgba_u32(6, 8, 12, 200));
    canvas.fill_rounded_rect(layout.modal_rect, if state.fullscreen { 0.0 } else { 8.0 }, rgba_u32(14, 18, 26, 250));
    if !state.fullscreen {
        canvas.stroke_rounded_rect(layout.modal_rect, 8.0, 1.5, rgba_u32(82, 130, 180, 220));
    }

    // Top bar
    canvas.fill_rect(layout.top_bar, rgba_u32(20, 26, 38, 255));
    let title_w = (layout.btn_system.x - layout.top_bar.x - 18.0).max(60.0);
    canvas.draw_text_in_rect(
        rasterizer,
        Rect::new(layout.top_bar.x + 12.0, layout.top_bar.y + 4.0, title_w, 26.0),
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

    draw_btn(canvas, rasterizer, layout.btn_system, "System Player", PlayerHitAction::OpenSystemPlayer, true);
    draw_btn(
        canvas,
        rasterizer,
        layout.btn_full,
        if state.fullscreen { "Exit" } else { "Full" },
        PlayerHitAction::ToggleFullscreen,
        true,
    );
    draw_btn(canvas, rasterizer, layout.btn_close, "X", PlayerHitAction::CloseModal, true);

    out_hits.push((layout.btn_system, PlayerHitAction::OpenSystemPlayer));
    out_hits.push((layout.btn_full, PlayerHitAction::ToggleFullscreen));
    out_hits.push((layout.btn_close, PlayerHitAction::CloseModal));

    // Video area
    canvas.fill_rect(layout.video_rect, rgba_u32(0, 0, 0, 255));
    if state.frame_w > 0 && state.frame_h > 0 && !state.frame_rgba.is_empty() {
        canvas.blit_rgba_aspect_fit(layout.video_rect, state.frame_w, state.frame_h, &state.frame_rgba);
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

    // Bottom bar
    canvas.fill_rect(layout.bottom_bar, rgba_u32(20, 26, 38, 255));
    draw_btn(
        canvas,
        rasterizer,
        layout.btn_play,
        if state.playing { "Pause" } else { "Play" },
        PlayerHitAction::TogglePlay,
        true,
    );
    draw_btn(canvas, rasterizer, layout.btn_back, "-5s", PlayerHitAction::SeekBack, true);
    draw_btn(canvas, rasterizer, layout.btn_fwd, "+5s", PlayerHitAction::SeekForward, true);
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
            Rect::new(layout.scrub_rect.x, layout.scrub_rect.y, fill_w, layout.scrub_rect.h),
            4.0,
            rgba_u32(95, 205, 255, 255),
        );
    }
    let time_str = format!("{} / {}", fmt_time(disp_sec), fmt_time(state.duration_sec));
    canvas.draw_text_in_rect(
        rasterizer,
        Rect::new(layout.scrub_rect.x, layout.bottom_bar.y + 28.0, layout.scrub_rect.w, 16.0),
        true,
        10.0,
        rgba_u32(190, 208, 230, 230),
        &time_str,
        true,
    );

    layout
}
