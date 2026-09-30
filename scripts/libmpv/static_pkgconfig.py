"""Publish static shader dependencies with their complete link closure."""
import pathlib
import sys

prefix = sys.argv[1]
pcdir = pathlib.Path(prefix) / "lib/pkgconfig"
pcdir.mkdir(parents=True, exist_ok=True)
shader_libs = "-lshaderc_combined -lglslang -lglslang-default-resource-limits -lSPIRV-Tools-opt -lSPIRV-Tools"
cross_libs = "-lspirv-cross-c -lspirv-cross-hlsl -lspirv-cross-glsl -lspirv-cross-msl -lspirv-cross-cpp -lspirv-cross-reflect -lspirv-cross-core -lspirv-cross-util"
for name, version, libs, include in [
    ("shaderc", "2026.3.1", shader_libs, "include"),
    ("shaderc_combined", "2026.3.1", shader_libs, "include"),
    ("spirv-cross-c-shared", "0.68.0", cross_libs, "include/spirv_cross"),
]:
    for lib in libs.split():
        archive = pathlib.Path(prefix) / "lib" / ("lib" + lib[2:] + ".a")
        if not archive.is_file():
            raise SystemExit(f"Missing rebuilt shader archive: {archive}")
    (pcdir / (name + ".pc")).write_text(
        f"prefix={prefix}\nlibdir=${{prefix}}/lib\nincludedir=${{prefix}}/{include}\n"
        f"Name: {name}\nDescription: Aperture static shader dependency\nVersion: {version}\n"
        f"Libs: -L${{libdir}} {libs}\nCflags: -I${{includedir}}\n"
    )
