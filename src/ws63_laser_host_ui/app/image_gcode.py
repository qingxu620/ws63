"""Deterministic raster-to-G-code helpers used by the image workspace."""
from __future__ import annotations

from collections.abc import Sequence
import math


Point = tuple[int, int]
Contour = list[Point]
Mask = list[list[bool]]


def _validated_rows(rows: Sequence[Sequence[int]]) -> tuple[int, int]:
    if not rows or not rows[0]:
        raise ValueError("image rows must not be empty")
    width = len(rows[0])
    if any(len(row) != width for row in rows):
        raise ValueError("all image rows must have the same width")
    return width, len(rows)


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
    mask: Mask = [[False] * width for _ in range(height)]
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
        return [list(row) for row in mask]

    visited = [[False] * width for _ in range(height)]
    cleaned: Mask = [[False] * width for _ in range(height)]
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
                for nx, ny in (
                    (px - 1, py),
                    (px + 1, py),
                    (px, py - 1),
                    (px, py + 1),
                ):
                    if (
                        nx < 0
                        or ny < 0
                        or nx >= width
                        or ny >= height
                        or visited[ny][nx]
                        or not mask[ny][nx]
                    ):
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
    mask: Mask = [[int(value) <= limit for value in row] for row in rows]

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
    dark = [[int(value) <= limit for value in row] for row in rows]
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


def vector_contours_to_gcode(
    contours: Sequence[Sequence[Point]],
    image_width: int,
    image_height: int,
    width_mm: float,
    speed_mm_min: int,
    height_mm: float | None = None,
    power_s: int = 1000,
) -> str:
    """Convert closed pixel-space contours into continuous outline toolpaths."""
    if image_width <= 0 or image_height <= 0:
        raise ValueError("image dimensions must be greater than zero")
    if width_mm <= 0:
        raise ValueError("width_mm must be greater than zero")
    if height_mm is not None and height_mm <= 0:
        raise ValueError("height_mm must be greater than zero")
    if speed_mm_min <= 0:
        raise ValueError("speed_mm_min must be greater than zero")
    mark_power = max(0, min(1000, int(power_s)))

    step = width_mm / image_width
    if height_mm is not None:
        step = min(step, height_mm / image_height)
    pending = [list(contour[:-1]) for contour in contours if len(contour) >= 4]
    current: Point = (0, 0)
    ordered: list[Contour] = []
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

    lines = ["G90", "M3 S0", f"F{int(speed_mm_min)}"]
    for contour in ordered:
        start_x = contour[0][0] * step
        start_y = contour[0][1] * step
        lines.append(f"M5")  # Ensure laser off before rapid move
        lines.append(f"G0 X{start_x:.3f} Y{start_y:.3f}")
        first_mark = True
        for x, y in contour[1:]:
            if first_mark:
                lines.append(f"M3 S{mark_power}")
                lines.append(f"G1 X{x * step:.3f} Y{y * step:.3f}")
                first_mark = False
            else:
                lines.append(f"G1 X{x * step:.3f} Y{y * step:.3f}")
        lines.append(f"M5")  # Turn off laser after contour
        lines.append(f"G0 X{contour[-1][0] * step:.3f} Y{contour[-1][1] * step:.3f}")
    lines.extend(("M5", "M30"))
    return "\n".join(lines) + "\n"


def _raster_runs_to_gcode(
    runs_by_row: Sequence[Sequence[tuple[int, int]]],
    image_width: int,
    image_height: int,
    width_mm: float,
    speed_mm_min: int,
    height_mm: float | None = None,
    power_s: int = 1000,
) -> str:
    if width_mm <= 0:
        raise ValueError("width_mm must be greater than zero")
    if height_mm is not None and height_mm <= 0:
        raise ValueError("height_mm must be greater than zero")
    if speed_mm_min <= 0:
        raise ValueError("speed_mm_min must be greater than zero")
    mark_power = max(0, min(1000, int(power_s)))

    x_step = width_mm / image_width
    if height_mm is not None:
        x_step = min(x_step, height_mm / image_height)
    y_step = x_step
    lines = [
        "G90",
        "M3 S0",
        f"F{int(speed_mm_min)}",
    ]
    for y, runs in enumerate(runs_by_row):
        ordered = runs if y % 2 == 0 else list(reversed(runs))
        for start, end in ordered:
            if y % 2 == 0:
                x0, x1 = start * x_step, (end + 1) * x_step
            else:
                x0, x1 = (end + 1) * x_step, start * x_step
            y_mm = y * y_step
            lines.append(f"M5")  # Ensure laser off before rapid move
            lines.append(f"G0 X{x0:.3f} Y{y_mm:.3f}")
            lines.append(f"M3 S{mark_power}")
            lines.append(f"G1 X{x1:.3f} Y{y_mm:.3f}")
    lines.extend(("M5", "M30"))
    return "\n".join(lines) + "\n"


def raster_mask_to_gcode(
    mask: Sequence[Sequence[bool]],
    width_mm: float,
    speed_mm_min: int,
    height_mm: float | None = None,
    power_s: int = 1000,
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
    )


def raster_rows_to_gcode(
    rows: Sequence[Sequence[int]],
    threshold: int,
    width_mm: float,
    speed_mm_min: int,
    height_mm: float | None = None,
    power_s: int = 1000,
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
    )
