"""Deterministic raster-to-G-code helpers used by the image workspace."""
from __future__ import annotations

from array import array
from collections.abc import Callable, Iterator, Sequence
from dataclasses import dataclass
from io import StringIO
import math
import random
from typing import TypeAlias


Point = tuple[float, float]
Contour = list[Point]
MaskRow: TypeAlias = list[bool] | bytearray
Mask = list[MaskRow]
GrayRow: TypeAlias = list[int] | bytearray
GrayRows: TypeAlias = list[GrayRow]
RgbPixel: TypeAlias = tuple[int, int, int] | tuple[int, int, int, int]
RgbRows: TypeAlias = Sequence[Sequence[RgbPixel]]


# These names intentionally mirror the vocabulary used by LaserGRBL's raster
# import dialog.  Keeping them as plain strings makes them safe to persist in
# the existing JSON/UI state without introducing an enum migration.
GRAYSCALE_FORMULAS = (
    "simple_average",
    "weight_average",
    "optical_correct",
    "custom",
)
DITHER_ALGORITHMS = (
    "floyd_steinberg",
    "atkinson",
    "burks",
    "jarvis",
    "random",
    "sierra2",
    "sierra3",
    "sierra_lite",
    "stucki",
)
SCAN_DIRECTIONS = ("horizontal", "vertical", "diagonal")


@dataclass(frozen=True)
class LaserGrblVectorOptions:
    """LaserGRBL/Potrace-style bitmap cleanup controls for outline extraction."""

    threshold: int = 128
    brightness: int = 100
    contrast: int = 100
    white_clip: int = 5
    black_clip: int = 0
    spot_remove_px: float = 4.0
    smoothing_px: float = 1.0
    optimize_paths: bool = True
    curve_smoothing: float = 0.0
    adaptive_quality: bool = False


@dataclass(frozen=True)
class LaserGrblVectorResult:
    rows: list[list[int]]
    mask: Mask
    raw_contours: list[Contour]
    contours: list[Contour]


@dataclass(frozen=True)
class RasterGcodeOptions:
    """Options shared by grayscale line-to-line and fill generation.

    LaserGRBL maps a black pixel to the configured maximum S value and a
    non-black pixel linearly into the S-min/S-max range.  White pixels are
    always travel/off pixels.  The options object keeps that mapping explicit
    and gives the UI a stable, testable contract.
    """

    width_mm: float
    speed_mm_min: int
    height_mm: float | None = None
    power_min_s: int = 0
    power_max_s: int = 1000
    laser_mode: str = "M4"
    x_offset_mm: float = 0.0
    y_offset_mm: float = 0.0
    direction: str = "horizontal"
    unidirectional: bool = False
    white_threshold: int = 255
    rapid_white: bool = True


def _normalise_formula(formula: str | int) -> str:
    if isinstance(formula, int):
        return GRAYSCALE_FORMULAS[max(0, min(len(GRAYSCALE_FORMULAS) - 1, formula))]
    value = str(formula).strip().lower().replace("-", "_").replace(" ", "_")
    aliases = {
        "average": "simple_average",
        "simple": "simple_average",
        "weight": "weight_average",
        "weighted": "weight_average",
        "optical": "optical_correct",
        "luma": "optical_correct",
    }
    value = aliases.get(value, value)
    return value if value in GRAYSCALE_FORMULAS else "optical_correct"


def grayscale_channel_weights(
    formula: str | int = "optical_correct",
    *,
    red_weight: float = 100.0,
    green_weight: float = 100.0,
    blue_weight: float = 100.0,
) -> tuple[float, float, float]:
    """Return normalized RGB weights for a supported grayscale formula."""

    selected = _normalise_formula(formula)
    if selected == "simple_average":
        return (1.0 / 3.0, 1.0 / 3.0, 1.0 / 3.0)
    if selected == "weight_average":
        return (1.0 / 3.0, 4.0 / 9.0, 2.0 / 9.0)
    if selected == "custom":
        return (
            max(0.0, float(red_weight)) / 300.0,
            max(0.0, float(green_weight)) / 300.0,
            max(0.0, float(blue_weight)) / 300.0,
        )
    return (0.299, 0.587, 0.114)


def rgb_rows_to_grayscale(
    rows: RgbRows,
    *,
    formula: str | int = "optical_correct",
    red_weight: float = 100.0,
    green_weight: float = 100.0,
    blue_weight: float = 100.0,
    invert: bool = False,
) -> GrayRows:
    """Convert RGB(A) rows to 8-bit grayscale using LaserGRBL formulas.

    Transparent pixels are composited over white, matching the way the
    original LaserGRBL importer clears alpha before thresholding.  Custom
    channel weights use LaserGRBL's percentage convention: 100/100/100 is
    neutral, while increasing one channel also increases its contribution.
    """

    if not rows or not rows[0]:
        raise ValueError("image rows must not be empty")
    width = len(rows[0])
    if any(len(row) != width for row in rows):
        raise ValueError("all image rows must have the same width")

    weights = grayscale_channel_weights(
        formula,
        red_weight=red_weight,
        green_weight=green_weight,
        blue_weight=blue_weight,
    )

    output: GrayRows = []
    for row in rows:
        result_row: list[int] = []
        for pixel in row:
            red, green, blue = (int(pixel[0]), int(pixel[1]), int(pixel[2]))
            if len(pixel) >= 4:
                alpha = max(0, min(255, int(pixel[3]))) / 255.0
                # Transparent/partially transparent source is treated as a
                # white canvas rather than producing a black halo.
                red = round(red * alpha + 255.0 * (1.0 - alpha))
                green = round(green * alpha + 255.0 * (1.0 - alpha))
                blue = round(blue * alpha + 255.0 * (1.0 - alpha))
            gray = _clamp_u8(
                red * weights[0] + green * weights[1] + blue * weights[2]
            )
            result_row.append(255 - gray if invert else gray)
        output.append(result_row)
    return output


def invert_gray_rows(rows: Sequence[Sequence[int]]) -> GrayRows:
    """Return the photographic negative of grayscale rows."""

    _validated_rows(rows)
    return [[255 - _clamp_u8(value) for value in row] for row in rows]


def rotate_gray_rows(rows: Sequence[Sequence[int]], quarter_turns: int) -> GrayRows:
    """Rotate grayscale rows clockwise in 90-degree increments."""

    width, height = _validated_rows(rows)
    turns = int(quarter_turns) % 4
    result = [[_clamp_u8(value) for value in row] for row in rows]
    for _ in range(turns):
        result = [
            [result[height - 1 - y][x] for y in range(height)]
            for x in range(width)
        ]
        height, width = width, height
    return result


def flip_gray_rows(
    rows: Sequence[Sequence[int]],
    *,
    horizontal: bool = False,
    vertical: bool = False,
) -> GrayRows:
    """Flip grayscale rows without changing their dimensions."""

    _validated_rows(rows)
    result = [[_clamp_u8(value) for value in row] for row in rows]
    if horizontal:
        result = [list(reversed(row)) for row in result]
    if vertical:
        result = list(reversed(result))
    return result


def _validated_rows(rows: Sequence[Sequence[int]]) -> tuple[int, int]:
    if not rows or not rows[0]:
        raise ValueError("image rows must not be empty")
    width = len(rows[0])
    if any(len(row) != width for row in rows):
        raise ValueError("all image rows must have the same width")
    return width, len(rows)


def _clamp_u8(value: float) -> int:
    return max(0, min(255, int(round(value))))


def _validate_job_geometry(
    width_mm: float,
    height_mm: float | None,
    speed_mm_min: int,
    x_offset_mm: float,
    y_offset_mm: float,
) -> None:
    values = {
        "width_mm": width_mm,
        "speed_mm_min": speed_mm_min,
        "x_offset_mm": x_offset_mm,
        "y_offset_mm": y_offset_mm,
    }
    if height_mm is not None:
        values["height_mm"] = height_mm
    if any(not math.isfinite(float(value)) for value in values.values()):
        raise ValueError("job geometry values must be finite")
    if width_mm <= 0:
        raise ValueError("width_mm must be greater than zero")
    if height_mm is not None and height_mm <= 0:
        raise ValueError("height_mm must be greater than zero")
    if speed_mm_min <= 0:
        raise ValueError("speed_mm_min must be greater than zero")
    if x_offset_mm < 0 or y_offset_mm < 0:
        raise ValueError("job offsets must not be negative")


def apply_lasergrbl_tone(
    rows: Sequence[Sequence[int]],
    *,
    brightness: int = 100,
    contrast: int = 100,
    white_clip: int = 5,
    black_clip: int = 0,
) -> list[list[int]]:
    """Apply LaserGRBL-style brightness/contrast cleanup before thresholding."""
    _validated_rows(rows)
    brightness_shift = (
        max(0, min(200, int(brightness))) - 100
    ) * 255.0 / 100.0
    factor = max(0, min(200, int(contrast))) / 100.0
    clip = max(0, min(255, int(white_clip)))
    white_cutoff = 255 - clip
    black = max(0, min(255, int(black_clip)))

    # The transform only depends on an 8-bit input value.  Building the LUT
    # once and applying it with bytearray.translate avoids millions of Python
    # calls for high line-density raster jobs.
    lut = bytearray(256)
    for value in range(256):
        mapped = _clamp_u8(factor * value + brightness_shift)
        if mapped > white_cutoff:
            mapped = 255
        elif black > 0 and mapped <= black:
            mapped = 0
        lut[value] = mapped
    table = bytes(lut)
    return [bytearray(row).translate(table) for row in rows]


def trace_dark_runs(
    rows: Sequence[Sequence[int]], threshold: int
) -> list[list[tuple[int, int]]]:
    """Return inclusive horizontal runs whose grayscale value is dark enough."""
    _validated_rows(rows)
    limit = max(0, min(255, int(threshold)))
    traced: list[list[tuple[int, int]]] = []
    for row in rows:
        runs: list[tuple[int, int]] = []
        start: int | None = None
        for x, value in enumerate(row):
            is_dark = int(value) <= limit
            if is_dark and start is None:
                start = x
            elif not is_dark and start is not None:
                runs.append((start, x - 1))
                start = None
        if start is not None:
            runs.append((start, len(row) - 1))
        traced.append(runs)
    return traced


def _validated_mask(mask: Sequence[Sequence[bool]]) -> tuple[int, int]:
    if not mask or not mask[0]:
        raise ValueError("image mask must not be empty")
    width = len(mask[0])
    if any(len(row) != width for row in mask):
        raise ValueError("all image mask rows must have the same width")
    return width, len(mask)


def _build_integral_rows(rows: Sequence[Sequence[int]]) -> list[list[int]]:
    width, height = _validated_rows(rows)
    integral = [[0] * (width + 1) for _ in range(height + 1)]
    for y, row in enumerate(rows, start=1):
        row_sum = 0
        previous = integral[y - 1]
        current = integral[y]
        for x, value in enumerate(row, start=1):
            row_sum += int(value)
            current[x] = previous[x] + row_sum
    return integral


def _window_mean(
    integral: Sequence[Sequence[int]], x0: int, y0: int, x1: int, y1: int
) -> float:
    total = (
        integral[y1 + 1][x1 + 1]
        - integral[y0][x1 + 1]
        - integral[y1 + 1][x0]
        + integral[y0][x0]
    )
    return total / ((x1 - x0 + 1) * (y1 - y0 + 1))


def _edge_mask(
    rows: Sequence[Sequence[int]], edge_threshold: int
) -> Mask:
    width, height = _validated_rows(rows)
    threshold = max(0, int(edge_threshold))
    mask: Mask = [bytearray(width) for _ in range(height)]
    for y in range(height):
        y0 = max(0, y - 1)
        y1 = min(height - 1, y + 1)
        for x in range(width):
            x0 = max(0, x - 1)
            x1 = min(width - 1, x + 1)
            gradient = max(
                abs(int(rows[y][x1]) - int(rows[y][x0])),
                abs(int(rows[y1][x]) - int(rows[y0][x])),
            )
            if gradient >= threshold:
                mask[y][x] = True
    return mask


def _remove_small_components(mask: Sequence[Sequence[bool]], min_area_px: float) -> Mask:
    width, height = _validated_mask(mask)
    min_area = max(0, int(math.ceil(min_area_px)))
    if min_area <= 1:
        return [bytearray(bool(value) for value in row) for row in mask]

    visited = [bytearray(width) for _ in range(height)]
    cleaned: Mask = [bytearray(width) for _ in range(height)]
    for y in range(height):
        for x in range(width):
            if visited[y][x] or not mask[y][x]:
                continue
            stack = [(x, y)]
            visited[y][x] = True
            component: list[Point] = []
            while stack:
                px, py = stack.pop()
                component.append((px, py))
                for ny in range(max(0, py - 1), min(height, py + 2)):
                    for nx in range(max(0, px - 1), min(width, px + 2)):
                        if (nx, ny) == (px, py):
                            continue
                        if visited[ny][nx] or not mask[ny][nx]:
                            continue
                        visited[ny][nx] = True
                        stack.append((nx, ny))
            if len(component) >= min_area:
                for px, py in component:
                    cleaned[py][px] = True
    return cleaned


def prepare_laser_mask(
    rows: Sequence[Sequence[int]],
    threshold: int,
    *,
    adaptive: bool = True,
    edge_enhance: bool = False,
    min_area_px: float = 0,
    adaptive_radius: int = 8,
    adaptive_bias: int = 10,
    edge_threshold: int = 28,
) -> Mask:
    """Build a laser extraction mask from grayscale rows.

    The legacy path is a single global threshold. This helper keeps that base
    but can also recover mid-tone shapes from bright backgrounds and emphasize
    contrast edges before the G-code conversion stage.
    """
    width, height = _validated_rows(rows)
    limit = max(0, min(255, int(threshold)))
    mask: Mask = [
        bytearray(int(value) <= limit for value in row)
        for row in rows
    ]

    if adaptive:
        radius = max(1, int(adaptive_radius))
        bias = max(0, int(adaptive_bias))
        integral = _build_integral_rows(rows)
        for y, row in enumerate(rows):
            y0 = max(0, y - radius)
            y1 = min(height - 1, y + radius)
            for x, value in enumerate(row):
                local_mean = _window_mean(
                    integral,
                    max(0, x - radius),
                    y0,
                    min(width - 1, x + radius),
                    y1,
                )
                lightness = int(value)
                if lightness <= local_mean - bias and lightness <= 245:
                    mask[y][x] = True

    if edge_enhance:
        edges = _edge_mask(rows, edge_threshold=edge_threshold)
        for y in range(height):
            for x in range(width):
                if edges[y][x]:
                    mask[y][x] = True

    return _remove_small_components(mask, min_area_px)


_DITHER_KERNELS: dict[str, tuple[float, tuple[tuple[int, int, float], ...]]] = {
    "floyd_steinberg": (
        16.0,
        ((1, 0, 7), (-1, 1, 3), (0, 1, 5), (1, 1, 1)),
    ),
    "atkinson": (
        8.0,
        ((1, 0, 1), (2, 0, 1), (-1, 1, 1), (0, 1, 1), (1, 1, 1), (0, 2, 1)),
    ),
    "burks": (
        32.0,
        (
            (1, 0, 8),
            (2, 0, 4),
            (-2, 1, 2),
            (-1, 1, 4),
            (0, 1, 8),
            (1, 1, 4),
            (2, 1, 2),
        ),
    ),
    "jarvis": (
        48.0,
        (
            (1, 0, 7),
            (2, 0, 5),
            (-2, 1, 3),
            (-1, 1, 5),
            (0, 1, 7),
            (1, 1, 5),
            (2, 1, 3),
            (-2, 2, 1),
            (-1, 2, 3),
            (0, 2, 5),
            (1, 2, 3),
            (2, 2, 1),
        ),
    ),
    "sierra2": (
        16.0,
        (
            (1, 0, 4),
            (2, 0, 3),
            (-2, 1, 1),
            (-1, 1, 2),
            (0, 1, 3),
            (1, 1, 2),
            (2, 1, 1),
        ),
    ),
    "sierra3": (
        32.0,
        (
            (1, 0, 5),
            (2, 0, 3),
            (-2, 1, 2),
            (-1, 1, 4),
            (0, 1, 5),
            (1, 1, 4),
            (2, 1, 2),
            (-1, 2, 2),
            (0, 2, 3),
            (1, 2, 2),
        ),
    ),
    "sierra_lite": (
        4.0,
        ((1, 0, 2), (-1, 1, 1), (0, 1, 1)),
    ),
    "stucki": (
        42.0,
        (
            (1, 0, 8),
            (2, 0, 4),
            (-2, 1, 2),
            (-1, 1, 4),
            (0, 1, 8),
            (1, 1, 4),
            (2, 1, 2),
            (-2, 2, 1),
            (-1, 2, 2),
            (0, 2, 4),
            (1, 2, 2),
            (2, 2, 1),
        ),
    ),
}


def _normalise_dither_algorithm(algorithm: str) -> str:
    value = str(algorithm).strip().lower().replace("-", "_").replace(" ", "_")
    aliases = {
        "floyd": "floyd_steinberg",
        "fs": "floyd_steinberg",
        "jarvis_judice_ninke": "jarvis",
        "jjn": "jarvis",
        "sierra": "sierra3",
        "sierra_2": "sierra2",
        "sierra_3": "sierra3",
        "sierra_light": "sierra_lite",
        "sierra_lite": "sierra_lite",
    }
    value = aliases.get(value, value)
    return value if value in DITHER_ALGORITHMS else "floyd_steinberg"


def dither_rows_to_mask(
    rows: Sequence[Sequence[int]],
    threshold: int = 128,
    algorithm: str = "floyd_steinberg",
    *,
    serpentine: bool = False,
    random_seed: int = 0,
) -> Mask:
    """Convert grayscale rows to a LaserGRBL-compatible 1-bit dither mask.

    The available algorithms match the nine choices exposed by LaserGRBL.
    Error diffusion follows LaserGRBL's left-to-right row order by default;
    callers may opt into serpentine diffusion. ``random`` is deterministic for
    a given seed so previews and final G-code always agree.
    """

    width, height = _validated_rows(rows)
    limit = max(0, min(255, int(threshold)))
    selected = _normalise_dither_algorithm(algorithm)
    mask: Mask = [bytearray(width) for _ in range(height)]
    rng = random.Random(int(random_seed))

    if selected == "random":
        for y, row in enumerate(rows):
            for x, value in enumerate(row):
                value = float(_clamp_u8(value))
                if value >= 255.0:
                    mask[y][x] = False
                    continue
                if value <= 0.0:
                    mask[y][x] = True
                    continue
                # Random dithering varies the threshold around the selected BW
                # level.  A fixed seed keeps tests and the live preview stable.
                local_limit = max(
                    0.0,
                    min(255.0, limit + rng.uniform(-127.5, 127.5)),
                )
                mask[y][x] = value < local_limit
        return mask

    work = [array("f", (_clamp_u8(value) for value in row)) for row in rows]
    divisor, kernel = _DITHER_KERNELS[selected]
    for y in range(height):
        reverse = serpentine and y % 2 == 1
        x_values = range(width - 1, -1, -1) if reverse else range(width)
        for x in x_values:
            old = max(0.0, min(255.0, work[y][x]))
            new = 0.0 if old <= limit else 255.0
            mask[y][x] = new == 0.0
            error = old - new
            for dx, dy, weight in kernel:
                nx = x - dx if reverse else x + dx
                ny = y + dy
                if 0 <= nx < width and 0 <= ny < height:
                    work[ny][nx] += error * weight / divisor
    return mask


def centerline_mask(
    mask: Sequence[Sequence[bool]],
    max_iterations: int | None = None,
) -> Mask:
    """Thin a binary mask with Zhang-Suen-style iterations for centerline jobs."""
    source_width, source_height = _validated_mask(mask)
    # A white guard border is required for subjects touching the bitmap edge;
    # without it those edge pixels are never visited by the thinning loop.
    width, height = source_width + 2, source_height + 2
    thinned: Mask = [bytearray(width) for _ in range(height)]
    for y, row in enumerate(mask, start=1):
        for x, value in enumerate(row, start=1):
            thinned[y][x] = bool(value)

    def neighbors(x: int, y: int) -> list[bool]:
        return [
            thinned[y - 1][x],
            thinned[y - 1][x + 1],
            thinned[y][x + 1],
            thinned[y + 1][x + 1],
            thinned[y + 1][x],
            thinned[y + 1][x - 1],
            thinned[y][x - 1],
            thinned[y - 1][x - 1],
        ]

    iteration_limit = (
        max(width, height)
        if max_iterations is None
        else max(1, int(max_iterations))
    )

    def protected_removals(points: Sequence[Point]) -> set[tuple[int, int]]:
        """Keep one pixel when a thinning substep would erase a component."""

        removals = {(int(x), int(y)) for x, y in points}
        pending = set(removals)
        while pending:
            seed = pending.pop()
            group = {seed}
            stack = [seed]
            while stack:
                px, py = stack.pop()
                for ny in range(max(1, py - 1), min(height - 1, py + 2)):
                    for nx in range(max(1, px - 1), min(width - 1, px + 2)):
                        candidate = (nx, ny)
                        if candidate in pending:
                            pending.remove(candidate)
                            group.add(candidate)
                            stack.append(candidate)
            touches_survivor = any(
                thinned[ny][nx] and (nx, ny) not in removals
                for px, py in group
                for ny in range(max(1, py - 1), min(height - 1, py + 2))
                for nx in range(max(1, px - 1), min(width - 1, px + 2))
            )
            if not touches_survivor:
                center_x = sum(point[0] for point in group) / len(group)
                center_y = sum(point[1] for point in group) / len(group)
                keep = min(
                    group,
                    key=lambda point: (
                        (point[0] - center_x) ** 2 + (point[1] - center_y) ** 2,
                        point[1],
                        point[0],
                    ),
                )
                removals.remove(keep)
        return removals

    for _ in range(iteration_limit):
        changed = False
        for step in (0, 1):
            remove: list[Point] = []
            for y in range(1, height - 1):
                for x in range(1, width - 1):
                    if not thinned[y][x]:
                        continue
                    n = neighbors(x, y)
                    count = sum(1 for value in n if value)
                    if count < 2 or count > 6:
                        continue
                    transitions = sum(
                        1 for first, second in zip(n, n[1:] + n[:1])
                        if not first and second
                    )
                    if transitions != 1:
                        continue
                    if step == 0:
                        if n[0] and n[2] and n[4]:
                            continue
                        if n[2] and n[4] and n[6]:
                            continue
                    else:
                        if n[0] and n[2] and n[6]:
                            continue
                        if n[0] and n[4] and n[6]:
                            continue
                    remove.append((x, y))
            if remove:
                safe_remove = protected_removals(remove)
                changed = changed or bool(safe_remove)
                for x, y in safe_remove:
                    thinned[y][x] = False
        if not changed:
            break
    return [
        row[1:source_width + 1]
        for row in thinned[1:source_height + 1]
    ]


def trace_centerline_paths(mask: Sequence[Sequence[bool]]) -> list[list[Point]]:
    """Trace an 8-connected skeleton into continuous centerline polylines."""

    width, height = _validated_mask(mask)
    nodes = {
        (x, y)
        for y in range(height)
        for x in range(width)
        if mask[y][x]
    }
    if not nodes:
        return []

    def neighbours(point: tuple[int, int]) -> list[tuple[int, int]]:
        x, y = point
        values: list[tuple[int, int]] = []
        for ny in range(max(0, y - 1), min(height, y + 2)):
            for nx in range(max(0, x - 1), min(width, x + 2)):
                candidate = (nx, ny)
                if candidate == point or candidate not in nodes:
                    continue
                if (
                    nx != x
                    and ny != y
                    and ((nx, y) in nodes or (x, ny) in nodes)
                ):
                    # Do not add a diagonal shortcut around an existing
                    # orthogonal corner.  It would burn an extra diagonal at
                    # L bends and four spurious diagonals around a cross.
                    continue
                values.append(candidate)
        values.sort()
        return values

    neighbour_map = {point: neighbours(point) for point in nodes}
    visited_edges: set[
        tuple[tuple[int, int], tuple[int, int]]
    ] = set()

    def edge_key(
        first: tuple[int, int], second: tuple[int, int]
    ) -> tuple[tuple[int, int], tuple[int, int]]:
        return (first, second) if first <= second else (second, first)

    paths: list[list[Point]] = []

    for point in sorted(nodes, key=lambda value: (value[1], value[0])):
        if not neighbour_map[point]:
            paths.append([(float(point[0]), float(point[1]))])

    def trace(
        start: tuple[int, int],
        first: tuple[int, int],
    ) -> list[Point]:
        path: list[Point] = [
            (float(start[0]), float(start[1])),
            (float(first[0]), float(first[1])),
        ]
        visited_edges.add(edge_key(start, first))
        previous, current = start, first
        while True:
            candidates = [
                point
                for point in neighbour_map[current]
                if point != previous
                and edge_key(current, point) not in visited_edges
            ]
            if len(neighbour_map[current]) != 2 or not candidates:
                break
            following = candidates[0]
            visited_edges.add(edge_key(current, following))
            path.append((float(following[0]), float(following[1])))
            previous, current = current, following
            if current == start:
                break
        return path

    # End points and junctions split the graph into meaningful strokes.
    for start in sorted(
        (point for point in nodes if len(neighbour_map[point]) != 2),
        key=lambda point: (point[1], point[0]),
    ):
        for first in neighbour_map[start]:
            if edge_key(start, first) in visited_edges:
                continue
            path = trace(start, first)
            if len(path) >= 2:
                paths.append(path)

    # Any remaining edges are closed loops made exclusively of degree-2 nodes.
    for start in sorted(nodes, key=lambda point: (point[1], point[0])):
        for first in neighbour_map[start]:
            if edge_key(start, first) in visited_edges:
                continue
            path = trace(start, first)
            if path[-1] != path[0]:
                path.append(path[0])
            paths.append(path)
    def without_collinear_points(path: list[Point]) -> list[Point]:
        if len(path) < 3:
            return path

        def redundant(first: Point, middle: Point, last: Point) -> bool:
            first_dx = middle[0] - first[0]
            first_dy = middle[1] - first[1]
            second_dx = last[0] - middle[0]
            second_dy = last[1] - middle[1]
            return (
                first_dx * second_dy == first_dy * second_dx
                and first_dx * second_dx + first_dy * second_dy > 0
            )

        if path[0] == path[-1]:
            ring = path[:-1]
            kept = [
                point
                for index, point in enumerate(ring)
                if not redundant(
                    ring[index - 1],
                    point,
                    ring[(index + 1) % len(ring)],
                )
            ]
            return kept + [kept[0]] if kept else path

        kept = [path[0]]
        for index in range(1, len(path) - 1):
            if not redundant(kept[-1], path[index], path[index + 1]):
                kept.append(path[index])
        kept.append(path[-1])
        return kept

    return [without_collinear_points(path) for path in paths]


def centerline_paths_to_gcode(
    paths: Sequence[Sequence[Point]],
    image_width: int,
    image_height: int,
    width_mm: float,
    speed_mm_min: int,
    height_mm: float | None = None,
    power_s: int = 1000,
    laser_mode: str = "M4",
    x_offset_mm: float = 0.0,
    y_offset_mm: float = 0.0,
    optimize_paths: bool = True,
    rapid_travel: bool = True,
) -> str:
    """Convert skeleton polylines into continuous centerline G-code."""

    if image_width <= 0 or image_height <= 0:
        raise ValueError("image dimensions must be greater than zero")
    _validate_job_geometry(
        width_mm,
        height_mm,
        speed_mm_min,
        x_offset_mm,
        y_offset_mm,
    )
    step = width_mm / image_width
    if height_mm is not None:
        step = min(step, height_mm / image_height)
    mode = "M3" if str(laser_mode).upper() == "M3" else "M4"
    mark_power = max(0, min(1000, int(power_s)))
    pending = [list(path) for path in paths if path]
    if any(
        not math.isfinite(float(value))
        or x < 0
        or y < 0
        or x >= image_width
        or y >= image_height
        for path in pending
        for x, y in path
        for value in (x, y)
    ):
        raise ValueError("centerline path points must be finite and inside the image")
    ordered: list[list[Point]] = []
    current: Point = (float(image_width), 0.0)
    while pending:
        if not optimize_paths:
            ordered.extend(pending)
            break
        path_index, reverse = min(
            (
                (index, should_reverse)
                for index, path in enumerate(pending)
                for should_reverse in (False, True)
            ),
            key=lambda item: (
                (
                    pending[item[0]][-1 if item[1] else 0][0] - current[0]
                ) ** 2
                + (
                    pending[item[0]][-1 if item[1] else 0][1] - current[1]
                ) ** 2,
                item,
            ),
        )
        path = pending.pop(path_index)
        if reverse:
            path.reverse()
        ordered.append(path)
        current = path[-1]

    lines = [f"{mode} S0", f"F{int(speed_mm_min)}"]
    travel_command = "G0" if rapid_travel else "G1"
    for path in ordered:
        isolated = len(path) == 1
        first_pixel_x = path[0][0] + (0.25 if isolated else 0.5)
        first_x = x_offset_mm + (image_width - first_pixel_x) * step
        first_y = y_offset_mm + (path[0][1] + 0.5) * step
        lines.append(f"{travel_command} X{first_x:.3f} Y{first_y:.3f}")
        lines.append(f"S{mark_power}")
        if isolated:
            lines.append(
                f"G1 X{x_offset_mm + (image_width - (path[0][0] + 0.75)) * step:.3f} "
                f"Y{first_y:.3f}"
            )
        else:
            for x, y in path[1:]:
                lines.append(
                    f"G1 X{x_offset_mm + (image_width - (x + 0.5)) * step:.3f} "
                    f"Y{y_offset_mm + (y + 0.5) * step:.3f}"
                )
        lines.append("S0")
    if lines[-1] != "S0":
        lines.append("S0")
    lines.append("M5")
    return "\n".join(lines) + "\n"


def trace_dark_runs_from_mask(mask: Sequence[Sequence[bool]]) -> list[list[tuple[int, int]]]:
    """Return inclusive horizontal dark runs from a prepared boolean mask."""
    _validated_mask(mask)
    traced: list[list[tuple[int, int]]] = []
    for row in mask:
        runs: list[tuple[int, int]] = []
        start: int | None = None
        for x, is_dark in enumerate(row):
            if is_dark and start is None:
                start = x
            elif not is_dark and start is not None:
                runs.append((start, x - 1))
                start = None
        if start is not None:
            runs.append((start, len(row) - 1))
        traced.append(runs)
    return traced


def _polygon_area(contour: Sequence[Point]) -> float:
    if len(contour) < 4:
        return 0.0
    area2 = 0
    for first, second in zip(contour, contour[1:]):
        area2 += first[0] * second[1] - second[0] * first[1]
    return abs(area2) / 2.0


def _direction(first: Point, second: Point) -> Point:
    return second[0] - first[0], second[1] - first[1]


def _choose_boundary_step(previous: Point, current: Point, candidates: list[Point]) -> Point:
    """Keep the dark region on the right at ambiguous diagonal corners."""
    direction_index = {(1, 0): 0, (0, 1): 1, (-1, 0): 2, (0, -1): 3}
    incoming = direction_index[_direction(previous, current)]
    turn_priority = {1: 0, 0: 1, 3: 2, 2: 3}
    return min(
        candidates,
        key=lambda point: (
            turn_priority[(direction_index[_direction(current, point)] - incoming) % 4],
            point,
        ),
    )


def trace_vector_contours(
    rows: Sequence[Sequence[int]], threshold: int, min_area_px: float = 4.0
) -> list[Contour]:
    """Trace clockwise closed boundaries around thresholded dark regions."""
    limit = max(0, min(255, int(threshold)))
    dark = [bytearray(int(value) <= limit for value in row) for row in rows]
    return trace_vector_contours_from_mask(dark, min_area_px=min_area_px)


def trace_vector_contours_from_mask(
    dark: Sequence[Sequence[bool]], min_area_px: float = 4.0
) -> list[Contour]:
    """Trace clockwise closed boundaries around prepared dark regions."""
    width, height = _validated_mask(dark)
    edges: set[tuple[Point, Point]] = set()

    for y in range(height):
        for x in range(width):
            if not dark[y][x]:
                continue
            if y == 0 or not dark[y - 1][x]:
                edges.add(((x, y), (x + 1, y)))
            if x == width - 1 or not dark[y][x + 1]:
                edges.add(((x + 1, y), (x + 1, y + 1)))
            if y == height - 1 or not dark[y + 1][x]:
                edges.add(((x + 1, y + 1), (x, y + 1)))
            if x == 0 or not dark[y][x - 1]:
                edges.add(((x, y + 1), (x, y)))

    outgoing: dict[Point, list[Point]] = {}
    for start, end in edges:
        outgoing.setdefault(start, []).append(end)
    for candidates in outgoing.values():
        candidates.sort()

    remaining = set(edges)
    edge_stack = list(edges)
    contours: list[Contour] = []
    while remaining:
        while edge_stack and edge_stack[-1] not in remaining:
            edge_stack.pop()
        start, first_end = edge_stack.pop()
        remaining.remove((start, first_end))
        contour = [start, first_end]
        previous, current = start, first_end

        while current != start:
            candidates = [
                point for point in outgoing.get(current, [])
                if (current, point) in remaining
            ]
            if not candidates:
                break
            next_point = (
                candidates[0]
                if len(candidates) == 1
                else _choose_boundary_step(previous, current, candidates)
            )
            remaining.remove((current, next_point))
            contour.append(next_point)
            previous, current = current, next_point

        if contour[-1] == contour[0] and _polygon_area(contour) >= max(0.0, min_area_px):
            contours.append(contour)

    return contours


def _point_line_distance(point: Point, start: Point, end: Point) -> float:
    dx = end[0] - start[0]
    dy = end[1] - start[1]
    if dx == 0 and dy == 0:
        return math.hypot(point[0] - start[0], point[1] - start[1])
    numerator = abs(dy * point[0] - dx * point[1] + end[0] * start[1] - end[1] * start[0])
    return numerator / math.hypot(dx, dy)


def _rdp(points: Sequence[Point], tolerance: float) -> Contour:
    if len(points) <= 2 or tolerance <= 0:
        return list(points)
    farthest_index = 0
    farthest_distance = 0.0
    for index in range(1, len(points) - 1):
        distance = _point_line_distance(points[index], points[0], points[-1])
        if distance > farthest_distance:
            farthest_index = index
            farthest_distance = distance
    if farthest_distance <= tolerance:
        return [points[0], points[-1]]
    left = _rdp(points[:farthest_index + 1], tolerance)
    right = _rdp(points[farthest_index:], tolerance)
    return left[:-1] + right


def _remove_collinear_points(contour: Sequence[Point]) -> Contour:
    ring = list(contour[:-1])
    if len(ring) <= 3:
        return ring + [ring[0]] if ring else []
    kept: Contour = []
    for index, point in enumerate(ring):
        previous = ring[index - 1]
        following = ring[(index + 1) % len(ring)]
        first_dir = _direction(previous, point)
        second_dir = _direction(point, following)
        if first_dir[0] * second_dir[1] == first_dir[1] * second_dir[0]:
            continue
        kept.append(point)
    return kept + [kept[0]] if kept else []


def simplify_vector_contours(contours: Sequence[Sequence[Point]], tolerance_px: float) -> list[Contour]:
    """Remove grid collinearity and apply closed-ring RDP simplification."""
    simplified: list[Contour] = []
    for contour in contours:
        reduced = _remove_collinear_points(contour)
        ring = reduced[:-1]
        if len(ring) <= 3 or tolerance_px <= 0:
            if reduced:
                simplified.append(reduced)
            continue
        split = max(
            range(1, len(ring)),
            key=lambda index: (
                (ring[index][0] - ring[0][0]) ** 2
                + (ring[index][1] - ring[0][1]) ** 2
            ),
        )
        first_half = _rdp(ring[:split + 1], tolerance_px)
        second_half = _rdp(ring[split:] + [ring[0]], tolerance_px)
        combined = first_half[:-1] + second_half
        if len(combined) >= 4:
            simplified.append(combined)
    return simplified


def smooth_vector_contours(
    contours: Sequence[Sequence[Point]],
    strength: float = 1.0,
) -> list[list[tuple[float, float]]]:
    """Round bitmap stair-steps using closed-ring Chaikin corner cutting.

    Potrace fits smooth curves rather than emitting every bitmap corner.  This
    deterministic approximation is used when the optional curve-smoothing
    control is enabled; it preserves closed paths and works without a native
    Potrace runtime in the packaged Windows build.
    """

    amount = max(0.0, min(5.0, float(strength)))
    if amount <= 0:
        return [
            [(float(x), float(y)) for x, y in contour]
            for contour in contours
            if contour
        ]
    iterations = max(1, min(3, int(math.ceil(amount))))
    ratio = min(0.45, 0.18 + (amount % 1.0) * 0.20)
    output: list[list[tuple[float, float]]] = []
    for contour in contours:
        ring = [(float(x), float(y)) for x, y in contour[:-1]]
        if len(ring) < 3:
            if contour:
                output.append([(float(x), float(y)) for x, y in contour])
            continue
        for _ in range(iterations):
            smoothed: list[tuple[float, float]] = []
            for first, second in zip(ring, ring[1:] + ring[:1]):
                smoothed.append(
                    (
                        first[0] * (1.0 - ratio) + second[0] * ratio,
                        first[1] * (1.0 - ratio) + second[1] * ratio,
                    )
                )
                smoothed.append(
                    (
                        first[0] * ratio + second[0] * (1.0 - ratio),
                        first[1] * ratio + second[1] * (1.0 - ratio),
                    )
                )
            ring = smoothed
        output.append(ring + [ring[0]])
    return output


def lasergrbl_style_vectorize(
    rows: Sequence[Sequence[int]],
    options: LaserGrblVectorOptions,
) -> LaserGrblVectorResult:
    """Trace outlines with a LaserGRBL/Potrace-like preprocessing pipeline.

    LaserGRBL feeds a thresholded bitmap into Potrace after brightness/contrast
    cleanup, spot removal, smoothing, and optional optimization. This project
    keeps its existing integer contour tracer for deterministic RX-compatible
    G-code, while exposing the same practical controls at the Host layer.
    """
    adjusted_rows = apply_lasergrbl_tone(
        rows,
        brightness=options.brightness,
        contrast=options.contrast,
        white_clip=options.white_clip,
        black_clip=options.black_clip,
    )
    mask = prepare_laser_mask(
        adjusted_rows,
        options.threshold,
        adaptive=False,
        edge_enhance=False,
        min_area_px=options.spot_remove_px,
    )
    raw_contours = trace_vector_contours_from_mask(
        mask,
        min_area_px=options.spot_remove_px,
    )
    tolerance = max(0.0, float(options.smoothing_px))
    if options.adaptive_quality:
        width, height = _validated_rows(adjusted_rows)
        # Keep more detail for small sources and simplify very large sources.
        tolerance *= max(0.5, min(2.0, max(width, height) / 320.0))
    simplified = simplify_vector_contours(raw_contours, tolerance_px=tolerance)
    contours = smooth_vector_contours(
        simplified,
        strength=options.curve_smoothing,
    )
    return LaserGrblVectorResult(
        rows=adjusted_rows,
        mask=mask,
        raw_contours=raw_contours,
        contours=contours,
    )


def vector_contours_to_gcode(
    contours: Sequence[Sequence[Point]],
    image_width: int,
    image_height: int,
    width_mm: float,
    speed_mm_min: int,
    height_mm: float | None = None,
    power_s: int = 1000,
    laser_mode: str = "M4",
    x_offset_mm: float = 0.0,
    y_offset_mm: float = 0.0,
    optimize_paths: bool = True,
    rapid_travel: bool = True,
) -> str:
    """Convert closed pixel-space contours into continuous outline toolpaths."""
    if image_width <= 0 or image_height <= 0:
        raise ValueError("image dimensions must be greater than zero")
    _validate_job_geometry(
        width_mm,
        height_mm,
        speed_mm_min,
        x_offset_mm,
        y_offset_mm,
    )
    mark_power = max(0, min(1000, int(power_s)))
    mode = "M3" if str(laser_mode).upper() == "M3" else "M4"

    step = width_mm / image_width
    if height_mm is not None:
        step = min(step, height_mm / image_height)
    pending = [list(contour[:-1]) for contour in contours if len(contour) >= 4]
    if any(
        not math.isfinite(float(value))
        or x < 0
        or y < 0
        or x > image_width
        or y > image_height
        for contour in pending
        for x, y in contour
        for value in (x, y)
    ):
        raise ValueError("vector contour points must be finite and inside the image")
    # The machine's X axis is opposite to image-space X.  Start path ordering
    # from the image edge that maps to machine X=0 as well.
    current: Point = (image_width, 0)
    ordered: list[Contour] = []
    if optimize_paths:
        while pending:
            contour_index, start_index = min(
                (
                    (contour_index, point_index)
                    for contour_index, contour in enumerate(pending)
                    for point_index, point in enumerate(contour)
                ),
                key=lambda item: (
                    (pending[item[0]][item[1]][0] - current[0]) ** 2
                    + (pending[item[0]][item[1]][1] - current[1]) ** 2,
                    item,
                ),
            )
            ring = pending.pop(contour_index)
            rotated = ring[start_index:] + ring[:start_index]
            rotated.append(rotated[0])
            ordered.append(rotated)
            current = rotated[-1]
    else:
        ordered = [contour + [contour[0]] for contour in pending if contour]

    lines = [f"{mode} S0", f"F{int(speed_mm_min)}"]
    travel_command = "G0" if rapid_travel else "G1"
    current_power = 0

    def emit_power(power: int) -> None:
        nonlocal current_power
        if current_power != power:
            lines.append(f"S{power}")
            current_power = power

    for contour in ordered:
        start_x = x_offset_mm + (image_width - contour[0][0]) * step
        start_y = y_offset_mm + contour[0][1] * step
        emit_power(0)
        lines.append(f"{travel_command} X{start_x:.3f} Y{start_y:.3f}")
        emit_power(mark_power)
        for x, y in contour[1:]:
            lines.append(
                f"G1 X{x_offset_mm + (image_width - x) * step:.3f} "
                f"Y{y_offset_mm + y * step:.3f}"
            )
        emit_power(0)
    if lines[-1] != "S0":
        lines.append("S0")
    lines.append("M5")
    return "\n".join(lines) + "\n"


def _raster_runs_to_gcode(
    runs_by_row: Sequence[Sequence[tuple[int, int]]],
    image_width: int,
    image_height: int,
    width_mm: float,
    speed_mm_min: int,
    height_mm: float | None = None,
    power_s: int = 1000,
    laser_mode: str = "M4",
    x_offset_mm: float = 0.0,
    y_offset_mm: float = 0.0,
) -> str:
    if width_mm <= 0:
        raise ValueError("width_mm must be greater than zero")
    if height_mm is not None and height_mm <= 0:
        raise ValueError("height_mm must be greater than zero")
    if speed_mm_min <= 0:
        raise ValueError("speed_mm_min must be greater than zero")
    mark_power = max(0, min(1000, int(power_s)))
    mode = "M3" if str(laser_mode).upper() == "M3" else "M4"

    x_step = width_mm / image_width
    if height_mm is not None:
        x_step = min(x_step, height_mm / image_height)
    y_step = x_step
    output = StringIO()
    output.write(f"{mode} S0\nF{int(speed_mm_min)}\n")
    current_power = 0

    def emit_power(power: int) -> None:
        nonlocal current_power
        if current_power != power:
            output.write(f"S{power}\n")
            current_power = power

    for y, runs in enumerate(runs_by_row):
        ordered = runs if y % 2 == 0 else list(reversed(runs))
        for start, end in ordered:
            if y % 2 == 0:
                x0, x1 = start * x_step, (end + 1) * x_step
            else:
                x0, x1 = (end + 1) * x_step, start * x_step
            x0 = x_offset_mm + image_width * x_step - x0
            x1 = x_offset_mm + image_width * x_step - x1
            y_mm = y_offset_mm + y * y_step
            emit_power(0)
            output.write(f"G0 X{x0:.3f} Y{y_mm:.3f}\n")
            emit_power(mark_power)
            output.write(f"G1 X{x1:.3f} Y{y_mm:.3f}\n")
    if current_power == 0:
        output.write("S0\n")
    else:
        emit_power(0)
    output.write("M5\n")
    return output.getvalue()


def _normalise_scan_direction(direction: str) -> str:
    value = str(direction).strip().lower().replace("-", "_").replace(" ", "_")
    aliases = {
        "h": "horizontal",
        "x": "horizontal",
        "horizontal_scan": "horizontal",
        "v": "vertical",
        "y": "vertical",
        "vertical_scan": "vertical",
        "d": "diagonal",
        "45": "diagonal",
        "diagonal_45": "diagonal",
    }
    value = aliases.get(value, value)
    return value if value in SCAN_DIRECTIONS else "horizontal"


def _scan_pixel_lines(
    width: int,
    height: int,
    direction: str,
    *,
    unidirectional: bool,
) -> Iterator[list[Point]]:
    """Return ordered pixel paths for horizontal, vertical, or 45° scans."""

    selected = _normalise_scan_direction(direction)
    if selected == "horizontal":
        for y in range(height):
            line = [(x, y) for x in range(width)]
            if not unidirectional and y % 2:
                line.reverse()
            yield line
        return
    if selected == "vertical":
        for x in range(width):
            line = [(x, y) for y in range(height)]
            if not unidirectional and x % 2:
                line.reverse()
            yield line
        return

    for diagonal in range(width + height - 1):
        x0 = max(0, diagonal - height + 1)
        x1 = min(width - 1, diagonal)
        line = [(x, diagonal - x) for x in range(x0, x1 + 1)]
        if not unidirectional and diagonal % 2:
            line.reverse()
        yield line


def _grayscale_to_power(
    value: int,
    *,
    minimum: int,
    maximum: int,
    white_threshold: int,
) -> int:
    gray = _clamp_u8(value)
    if gray >= white_threshold:
        return 0
    darkness = 255 - gray
    if darkness <= 0 or maximum <= 0:
        return 0
    if maximum <= minimum:
        return maximum
    return max(
        0,
        min(
            maximum,
            int(round(minimum + darkness * (maximum - minimum) / 255.0)),
        ),
    )


def grayscale_power_rows(
    rows: Sequence[Sequence[int]],
    *,
    power_min_s: int = 0,
    power_max_s: int = 1000,
    white_threshold: int = 255,
) -> list[list[int]]:
    """Map grayscale values to the S range used by line-to-line engraving."""

    _validated_rows(rows)
    minimum = max(0, min(1000, int(power_min_s)))
    maximum = max(0, min(1000, int(power_max_s)))
    if minimum > maximum:
        minimum, maximum = maximum, minimum
    threshold = max(0, min(255, int(white_threshold)))
    power_lut = tuple(
        _grayscale_to_power(
            value,
            minimum=minimum,
            maximum=maximum,
            white_threshold=threshold,
        )
        for value in range(256)
    )
    return [[power_lut[int(value)] for value in row] for row in rows]


def grayscale_rows_to_gcode(
    rows: Sequence[Sequence[int]],
    width_mm: float,
    speed_mm_min: int,
    height_mm: float | None = None,
    power_min_s: int = 0,
    power_max_s: int = 1000,
    laser_mode: str = "M4",
    x_offset_mm: float = 0.0,
    y_offset_mm: float = 0.0,
    direction: str = "horizontal",
    unidirectional: bool = False,
    white_threshold: int = 255,
    rapid_white: bool = True,
    cancel_check: Callable[[], None] | None = None,
) -> str:
    """Generate LaserGRBL-style grayscale PWM line-to-line G-code.

    Adjacent pixels with the same mapped power are collapsed into a single
    move.  White/off regions are emitted as rapid travel, which keeps generated
    files substantially smaller without placing a conversion-stage size cap.
    """

    width, height = _validated_rows(rows)
    _validate_job_geometry(
        width_mm,
        height_mm,
        speed_mm_min,
        x_offset_mm,
        y_offset_mm,
    )

    x_step = float(width_mm) / width
    if height_mm is not None:
        x_step = min(x_step, float(height_mm) / height)
    y_step = x_step
    mode = "M3" if str(laser_mode).upper() == "M3" else "M4"
    minimum = max(0, min(1000, int(power_min_s)))
    maximum = max(0, min(1000, int(power_max_s)))
    if minimum > maximum:
        minimum, maximum = maximum, minimum
    off_threshold = max(0, min(255, int(white_threshold)))
    paths = _scan_pixel_lines(
        width,
        height,
        direction,
        unidirectional=bool(unidirectional),
    )

    output = StringIO()
    output.write(f"{mode} S0\nF{int(speed_mm_min)}\n")
    current_power = 0
    current_xy: tuple[float, float] | None = None
    current_motion = ""
    travel_command = "G0" if rapid_white else "G1"
    power_lut = tuple(
        _grayscale_to_power(
            value,
            minimum=minimum,
            maximum=maximum,
            white_threshold=off_threshold,
        )
        for value in range(256)
    )

    def machine_point(pixel_x: float, pixel_y: float) -> tuple[float, float]:
        return (
            x_offset_mm + (width - pixel_x) * x_step,
            y_offset_mm + pixel_y * y_step,
        )

    def pixel_edge_points(path: Sequence[Point], index: int) -> tuple[tuple[float, float], tuple[float, float]]:
        x, y = path[index]
        if len(path) == 1:
            selected = _normalise_scan_direction(direction)
            if selected == "vertical":
                return machine_point(x + 0.5, y), machine_point(x + 0.5, y + 1)
            if selected == "diagonal":
                return machine_point(x, y + 1), machine_point(x + 1, y)
            return machine_point(x, y + 0.5), machine_point(x + 1, y + 0.5)

        if index + 1 < len(path):
            nx, ny = path[index + 1]
            dx, dy = nx - x, ny - y
        else:
            px, py = path[index - 1]
            dx, dy = x - px, y - py
        start = machine_point(
            x + 0.5 - dx * 0.5,
            y + 0.5 - dy * 0.5,
        )
        end = machine_point(
            x + 0.5 + dx * 0.5,
            y + 0.5 + dy * 0.5,
        )
        return start, end

    def emit_move(
        command: str,
        point: tuple[float, float],
        power: int,
        *,
        modal_axes: bool = False,
    ) -> None:
        nonlocal current_xy, current_power, current_motion
        if current_xy is not None and (
            abs(current_xy[0] - point[0]) < 0.0005
            and abs(current_xy[1] - point[1]) < 0.0005
        ):
            if current_power != power:
                output.write(f"S{power}\n")
                current_power = power
            return
        words: list[str] = []
        if command != current_motion:
            words.append(command)
            current_motion = command
        if (
            not modal_axes
            or current_xy is None
            or abs(current_xy[0] - point[0]) >= 0.0005
        ):
            words.append(f"X{point[0]:.3f}")
        if (
            not modal_axes
            or current_xy is None
            or abs(current_xy[1] - point[1]) >= 0.0005
        ):
            words.append(f"Y{point[1]:.3f}")
        if current_power != power or power > 0:
            words.append(f"S{power}")
        output.write(" ".join(words) + "\n")
        current_xy = point
        current_power = power

    for path_index, path in enumerate(paths):
        if cancel_check is not None and path_index % 8 == 0:
            cancel_check()
        if not path:
            continue
        start = 0
        while start < len(path):
            x, y = path[start]
            power = power_lut[int(rows[y][x])]
            end = start + 1
            while end < len(path):
                nx, ny = path[end]
                next_power = power_lut[int(rows[ny][nx])]
                if next_power != power:
                    break
                end += 1

            run_start, _ = pixel_edge_points(path, start)
            _, run_end = pixel_edge_points(path, end - 1)
            if power > 0:
                if current_xy is None or (
                    abs(current_xy[0] - run_start[0]) >= 0.0005
                    or abs(current_xy[1] - run_start[1]) >= 0.0005
                ):
                    emit_move(travel_command, run_start, 0)
                emit_move("G1", run_end, power, modal_axes=True)
            start = end

        if unidirectional:
            if current_power != 0:
                output.write("S0\n")
                current_power = 0

    output.write("S0\n")
    output.write("M5\n")
    return output.getvalue()


def raster_options_to_gcode(
    rows: Sequence[Sequence[int]],
    options: RasterGcodeOptions,
) -> str:
    """Convenience wrapper for callers that persist ``RasterGcodeOptions``."""

    return grayscale_rows_to_gcode(
        rows,
        options.width_mm,
        options.speed_mm_min,
        height_mm=options.height_mm,
        power_min_s=options.power_min_s,
        power_max_s=options.power_max_s,
        laser_mode=options.laser_mode,
        x_offset_mm=options.x_offset_mm,
        y_offset_mm=options.y_offset_mm,
        direction=options.direction,
        unidirectional=options.unidirectional,
        white_threshold=options.white_threshold,
        rapid_white=options.rapid_white,
    )


def raster_mask_to_gcode(
    mask: Sequence[Sequence[bool]],
    width_mm: float,
    speed_mm_min: int,
    height_mm: float | None = None,
    power_s: int = 1000,
    laser_mode: str = "M4",
    x_offset_mm: float = 0.0,
    y_offset_mm: float = 0.0,
) -> str:
    """Convert a prepared dark mask into horizontal laser scan segments."""
    width, height = _validated_mask(mask)
    return _raster_runs_to_gcode(
        trace_dark_runs_from_mask(mask),
        width,
        height,
        width_mm,
        speed_mm_min,
        height_mm=height_mm,
        power_s=power_s,
        laser_mode=laser_mode,
        x_offset_mm=x_offset_mm,
        y_offset_mm=y_offset_mm,
    )


def raster_rows_to_gcode(
    rows: Sequence[Sequence[int]],
    threshold: int,
    width_mm: float,
    speed_mm_min: int,
    height_mm: float | None = None,
    power_s: int = 1000,
    laser_mode: str = "M4",
    x_offset_mm: float = 0.0,
    y_offset_mm: float = 0.0,
) -> str:
    """Convert grayscale rows into a bounded horizontal line-segment toolpath."""
    width, height = _validated_rows(rows)
    return _raster_runs_to_gcode(
        trace_dark_runs(rows, threshold),
        width,
        height,
        width_mm,
        speed_mm_min,
        height_mm=height_mm,
        power_s=power_s,
        laser_mode=laser_mode,
        x_offset_mm=x_offset_mm,
        y_offset_mm=y_offset_mm,
    )
