from pathlib import Path
import struct

from PySide6.QtGui import QImage


PROJECT_ROOT = Path(__file__).resolve().parents[1]


def test_windows_version_info_is_a_persistent_source_asset() -> None:
    spec_text = (PROJECT_ROOT / "WS63_Laser_Host.spec").read_text(encoding="utf-8")
    version_asset = PROJECT_ROOT / "assets" / "windows_version_info.txt"

    assert '"assets" / "windows_version_info.txt"' in spec_text
    assert version_asset.is_file()
    assert not version_asset.is_relative_to(PROJECT_ROOT / "build")


def test_build_script_uses_the_product_shortcut_name() -> None:
    script_text = (PROJECT_ROOT / "build_windows.ps1").read_text(encoding="utf-8-sig")
    spec_text = (PROJECT_ROOT / "WS63_Laser_Host.spec").read_text(encoding="utf-8")

    assert '"智绘星闪AI打标系统.lnk"' in script_text
    assert '$Shortcut.Description = "智绘星闪AI打标系统"' in script_text
    assert 'dist\\智绘星闪AI打标系统\\WS63_Laser_Host.exe' in script_text
    assert 'name="智绘星闪AI打标系统"' in spec_text


def test_windows_icon_uses_rounded_white_title_bar_safe_area() -> None:
    icon_bytes = (PROJECT_ROOT / "assets" / "app_icon.ico").read_bytes()
    build_script = (PROJECT_ROOT / "tools" / "build_icon.py").read_text(
        encoding="utf-8"
    )

    assert icon_bytes.startswith(b"\x00\x00\x01\x00")
    image_count = struct.unpack_from("<H", icon_bytes, 4)[0]
    entry_offset = 6 + (image_count - 1) * 16
    payload_size, payload_offset = struct.unpack_from("<II", icon_bytes, entry_offset + 8)
    image = QImage.fromData(
        icon_bytes[payload_offset : payload_offset + payload_size],
        "PNG",
    )

    assert image.size().width() == 256
    assert image.pixelColor(0, 0).alpha() == 0
    assert image.pixelColor(128, 2).alpha() == 255
    assert image.pixelColor(128, 2).red() == 255
    assert "image.fill(Qt.GlobalColor.transparent)" in build_script
    assert "addRoundedRect" in build_script
    assert "fillPath(tile_path, Qt.GlobalColor.white)" in build_script
    assert "content_size = max(1, round(size * 0.82))" in build_script
