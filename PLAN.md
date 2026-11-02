# FPSHUD — Rencana Implementasi & Spesifikasi (PLAN.md)

Proyek **FPSHUD** adalah *in-game performance overlay* minimalis dan *In-Game GUI Configurator* untuk Linux (Vulkan & OpenGL) berbasis **Dear ImGui**, dirancang khusus dan dioptimalkan untuk sistem **Intel Core i3-12100F** & **NVIDIA GeForce GTX 1660 SUPER**.

## 1. Environment Variable Tunggal (`FPS_HUD=1` / `FPS_HUD=0`)
- `FPS_HUD=1` : Mengaktifkan HUD Overlay sekaligus *In-Game GUI Configurator*.
- `FPS_HUD=0` (atau tidak di-set): Menonaktifkan FPSHUD sepenuhnya (*zero-overhead bypass*).
- Seluruh konfigurasi disimpan secara otomatis (*auto-save*) dalam format `key=value` murni di `~/.config/fpshud/config.ini` dan `~/.config/fpshud/custom_preset.ini`.

## 2. Fitur Unggulan In-Game GUI (`FPSHUD Configuration Tool`)
- **4 Preset Siap Pakai**: `Minimal`, `Standard`, `Advance`, dan `Custom` (`Save to Custom`).
- **Dynamic Color Coding**: Pewarnaan otomatis per kategori (`FPS` Kuning, `GPU` Hijau NVIDIA, `CPU` Biru Cyan, `RAM` Ungu, `SYS` Perak) + indikator peringatan suhu (`>= 72°C` Oranye, `>= 82°C` Merah).
- **Mini Frametime Graph di HUD**: Grafik gelombang *frametime (ms)* langsung di bawah HUD Overlay.
- **Benchmark Recorder ke CSV (`F9`)**: Merekam durasi, Average FPS, 1% Low, 0.1% Low, Min/Max FPS, suhu maksimum, dan daya maksimum ke `~/Dokumen/fpshud_benchmark.csv`.
- **Custom Keybindings**: Pengaturan tombol shortcut untuk Toggle GUI (`Insert/F12/F10/Home`), Toggle HUD (`F11/F8/End`), dan Benchmark (`F9/F7/PageUp`).

## 3. Daftar 19 Sensor Hardware & Sistem
- **Grup `FPS`**: `FPS Counter`, `FPS Average`, `FPS 1% Low`, `FPS 0.1% Low`, `Frame Time (ms)`
- **Grup `GPU` (NVML)**: `GPU Load (%)`, `GPU Clock (MHz)`, `GPU VRAM (GB)`, `GPU Bandwidth (GB/s)`, `GPU Temperature (C)`, `GPU Fan Speed (%)`, `GPU Power (W)`
- **Grup `CPU` (sysfs/procfs)**: `CPU Load (%)`, `CPU Clock (MHz)`, `CPU Power (W)`, `CPU Temperature (C)`
- **Grup `RAM` (procfs)**: `System RAM (GB)`
- **Grup `SYS`**: `Render API (VULKAN / OPENGL + Driver)`, `System Clock (HH:MM)`
