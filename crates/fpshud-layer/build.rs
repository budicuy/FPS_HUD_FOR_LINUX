use std::path::PathBuf;

fn main() {
    let manifest_dir = PathBuf::from(env!("CARGO_MANIFEST_DIR"));
    let version_script = manifest_dir
        .join("../../manifest/libfpshud.version")
        .canonicalize()
        .expect("missing manifest/libfpshud.version");
    println!("cargo:rerun-if-changed={}", version_script.display());
    println!("cargo:rerun-if-changed=../../shaders/quad.vert.spv");
    println!("cargo:rerun-if-changed=../../shaders/quad.frag.spv");
    println!(
        "cargo:rustc-cdylib-link-arg=-Wl,--version-script={}",
        version_script.display()
    );
}
