"""Visual editor for Super Flashy Birds levels (levels/*.txt).

Run:   python tools/level_editor.py              (opens level1.txt)
       python tools/level_editor.py levels/level2.txt

Needs Pillow (pip install pillow). It reads and writes the same text maps that
tools/build_levels.py builds; see levels/README.md for what each block does.
"""

import re
import sys
import tempfile
import tkinter as tk
from pathlib import Path
from tkinter import filedialog, messagebox, simpledialog, ttk

from PIL import Image, ImageDraw, ImageTk

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
import build_levels as levels  # noqa: E402

TILE = levels.TILE
ART = levels.ROOT / "include" / "art" / "super-flashy-birds"
ICON = Path(__file__).resolve().with_suffix(".ico")
ARROWS = levels.ARROWS            # "RLUD"
ANGLES = (0, 180, 90, 270)        # counterclockwise, from art pointing right
BOXES = "BMmb"
SPRITE_CHARS = "SNGgFOICcPT"
ZOOMS = (1, 2, 3, 4)
UNDO_LIMIT = 200
NEW_WIDTH, NEW_HEIGHT = 60, 15

PALETTE = [
    ("Terrain", [("X", "Grass"), ("H", "Dirt (background)"),
                 ("Y", "Yellow grass"), (".", "Eraser (air)")]),
    ("Direction blocks", [("R", "Right"), ("L", "Left"), ("U", "Up"),
                          ("D", "Down")]),
    ("Boxes", [("B", "Box"), ("M", "Mystery box"), ("m", "Magic bean box"),
               ("b", "Coin brick")]),
    ("Throwables", [("P", "Cardboard box"), ("T", "Tennis ball")]),
    ("Characters", [("S", "Player start"), ("N", "Nest (goal)"),
                    ("G", "Gorilla"), ("g", "Red gorilla"), ("F", "Fox")]),
    ("Collectibles", [("C", "Coin"), ("O", "Golden egg"),
                      ("I", "Infinity egg"), ("c", "Green egg (life)")]),
]
NAMES = {ch: name for _, items in PALETTE for ch, name in items}
NAMES.update({d: f"Bean spot for sublevel {d}" for d in "0123456789"})


# --------------------------------------------------------------------- files

class Section:
    """One map: the main level (number None) or a sublevel."""

    def __init__(self, number, grid, comments=()):
        self.number = number
        self.grid = grid                  # list of rows (lists), top row first
        self.comments = list(comments)    # comment lines written above it

    @property
    def rows(self):
        return len(self.grid)

    @property
    def width(self):
        return len(self.grid[0])

    @classmethod
    def blank(cls, number, width, height):
        grid = [["."] * width for _ in range(height)]
        grid[-1] = ["X"] * width
        if height > 1:
            grid[-2][2] = "S"
            grid[-2][width - 3] = "N"
        return cls(number, grid)


def make_grid(lines):
    lines = [ln.rstrip().replace(" ", ".") for ln in lines]
    while lines and not lines[0]:
        lines.pop(0)
    while lines and not lines[-1]:
        lines.pop()
    lines = lines or ["."]
    width = max(len(ln) for ln in lines) or 1
    return [list(ln.ljust(width, ".")) for ln in lines]


def read_level(text):
    """Split a level file into Sections, keeping its comments."""
    parts = [[None, [], []]]  # number, comments above, map lines
    pending = []              # comments not yet attached to anything
    for line in text.splitlines():
        if line.lstrip().startswith("#"):
            pending.append(line.rstrip())
            continue
        stripped = line.rstrip()
        current = parts[-1]
        if re.fullmatch(r"\d+", stripped) and any(l.strip() for l in parts[0][2]):
            parts.append([int(stripped), pending, []])
            pending = []
            continue
        if stripped and not any(l.strip() for l in current[2]):
            current[1] += pending
            pending = []
        current[2].append(stripped)
    sections = [Section(n, make_grid(lines), comments)
                for n, comments, lines in parts]
    sections[-1].comments += pending
    return sections


def write_level(sections):
    out = []
    for i, section in enumerate(sections):
        if i:
            out.append("")
        out += section.comments
        if section.number is not None:
            out.append(str(section.number))
        out += ["".join(row) for row in section.grid]
    return "\n".join(out) + "\n"


def check_level(text):
    """(error or None, warnings), using the same checks as the build."""
    with tempfile.TemporaryDirectory() as folder:
        path = Path(folder) / "check.txt"
        path.write_text(text, encoding="utf-8")
        try:
            _, warnings = levels.parse(path)
        except levels.LevelError as err:
            return str(err), []
    return None, warnings


# ------------------------------------------------------------------- drawing

def load(name):
    return Image.open(ART / name).convert("RGBA")


def shaded(image, factor):
    """A copy with row y darkened to factor(x, y) of its brightness."""
    out = image.copy()
    px = out.load()
    for y in range(out.height):
        for x in range(out.width):
            r, g, b, a = px[x, y]
            k = factor(x, y)
            px[x, y] = (int(r * k), int(g * k), int(b * k), a)
    return out


class Art:
    def __init__(self):
        self.sky = load("Sky.png")
        self.tiles = {
            "grass": load("GrassTile.png"), "cloud": load("CloudTile.png"),
            "dirt": load("DirtTile.png"),
            "cloudbg": load("CloudBackgroundTile.png"),
            "yellow": load("YellowGrassTile.png"),
        }
        for key, name in (("arrow", "DirectionalBlock.png"),
                          ("grassy", "GrassyDirectionalBlock.png"),
                          ("white", "WhiteDirectionalBlock.png")):
            source = load(name)
            for ch, angle in zip(ARROWS, ANGLES):
                self.tiles[key + ch] = source.rotate(angle, expand=True)
        box, mystery = load("Box.png"), load("MysteryBox.png")
        bean_box = mystery.copy()  # the game draws it like M; mark it here
        small = load("MagicBean.png").resize((9, 9), Image.NEAREST)
        bean_box.alpha_composite(small, (7, 7))
        coin_box = box.copy()  # the game draws it like B; mark it here
        coin = load("Coin.png").resize((9, 9), Image.NEAREST)
        coin_box.alpha_composite(coin, (7, 7))
        self.sprites = {
            "B": box, "M": mystery, "m": bean_box, "b": coin_box,
            "P": load("CardboardBox.png"), "T": load("TennisBall.png"),
            "S": load("FlashyBirdStandStill.png"), "N": load("Nest.png"),
            "G": load("Gorilla.png"), "g": load("RedGorilla.png"),
            "F": load("Fox.png"), "C": load("Coin.png"),
            "O": load("GoldenEgg.png"), "I": load("InfinityEgg.png"),
            "c": load("GreenEgg.png"),
        }
        for digit in "0123456789":
            spot = Image.new("RGBA", (TILE, TILE), (0, 0, 0, 0))
            draw = ImageDraw.Draw(spot)
            draw.rectangle((2, 1, 13, 14), fill=(20, 110, 40, 220),
                           outline=(255, 255, 255, 255))
            draw.text((5, 2), digit, fill=(255, 255, 255, 255))
            self.sprites[digit] = spot
        self._dirt_cache = {}

    def dirt(self, key, under_grass, sky_sides):
        """A background tile with the shading the game gives it."""
        cache_key = (key, under_grass, sky_sides)
        if cache_key not in self._dirt_cache:
            def factor(px, py):
                already = (2 if py < 5 else 1 if py < 9 else 0) if under_grass else 0
                nearest = TILE
                for dc, dr in sky_sides:
                    dx = px if dc < 0 else TILE - 1 - px if dc > 0 else 0
                    dy = py if dr > 0 else TILE - 1 - py if dr < 0 else 0
                    nearest = min(nearest, max(dx, dy))
                depth = 2 if nearest < 3 else 1 if nearest < 6 else 0
                if depth > already:
                    return (9 if depth == 2 else 12) / 16
                return (1, 0.75, 0.55)[already]
            self._dirt_cache[cache_key] = shaded(self.tiles[key], factor)
        return self._dirt_cache[cache_key]


def tile_keys(section):
    """What the terrain layer shows in each cell, as {(x, y): key}; y counts
    down from the top. Follows drawGround() in super_flashy_birds.cpp."""
    grid, rows, width = section.grid, section.rows, section.width
    cloud = section.number is not None

    def cell(col, row):  # row 0 is the bottom
        if 0 <= col < width and 0 <= row < rows:
            return grid[rows - 1 - row][col]
        return "."

    dirt = {(c, r) for c in range(width) for r in range(rows)
            if levels.is_dirt(cell, c, r, width, rows)}
    solid, arrows, yellow = [], [], []
    for col in range(width):
        s_mask = y_mask = 0
        a_masks = [0] * len(ARROWS)
        for row in range(rows):
            ch = cell(col, row)
            if ch in "XY" + ARROWS:
                s_mask |= 1 << row
            if ch in ARROWS:
                a_masks[ARROWS.index(ch)] |= 1 << row
            elif ch == "Y":
                a_masks[ARROWS.index("U")] |= 1 << row
                y_mask |= 1 << row
        solid.append(s_mask)
        arrows.append(tuple(a_masks))
        yellow.append(y_mask)
    grassy = levels.grassy_masks(solid, arrows, yellow)

    def is_grass(col, row):
        return cell(col, row) in ("X", "Y")

    def is_sky(col, row):
        if not (0 <= col < width and row >= 0):
            return False
        return cell(col, row) not in "XY" + ARROWS + BOXES and (col, row) not in dirt

    keys = {}
    for col in range(width):
        for row in range(rows):
            ch = cell(col, row)
            key = None
            if ch in ("X", "Y"):
                edges = ""
                if ch == "Y" or not cloud:
                    above = cell(col, row + 1)
                    if (col, row + 1) in dirt or above in tuple(ARROWS + BOXES):
                        edges += "t"
                    if not is_grass(col, row - 1):
                        edges += "b"
                    if not is_grass(col - 1, row):
                        edges += "l"
                    if not is_grass(col + 1, row):
                        edges += "r"
                base = "yellow" if ch == "Y" else "cloud" if cloud else "grass"
                joins = "".join(
                    side for side, c, r in (("u", col, row + 1),
                                            ("d", col, row - 1),
                                            ("l", col - 1, row),
                                            ("r", col + 1, row))
                    if cell(c, r) == ch)
                key = ("grass", base, edges, joins)
            elif (col, row) in dirt:
                sides = tuple((dc, dr) for dr in (-1, 0, 1) for dc in (-1, 0, 1)
                              if (dc or dr) and is_sky(col + dc, row + dr))
                key = ("dirt", "cloudbg" if cloud else "dirt",
                       not cloud and is_grass(col, row + 1), sides)
            elif ch in tuple(ARROWS):
                look = ("white" if cloud else
                        "grassy" if (grassy[col] >> row) & 1 else "arrow")
                key = ("arrow", look + ch)
            if key:
                keys[(col, rows - 1 - row)] = key
    return keys


def joined(art, joins):
    """A grass or cloud tile redrawn where it meets another of its kind. Follows
    joinTile() in super_flashy_birds.cpp."""
    if not joins:
        return art
    source, out = art.load(), art.copy()
    px = out.load()
    base, stripe = source[2, 7], source[8, 7]
    for y in range(TILE):
        for x in range(TILE):
            if ("u" in joins and y <= 4) or ("d" in joins and y >= 11):
                px[x, y] = stripe if (x + y + 3) % 16 < 5 else base
            elif 11 <= y <= 14 and (("l" in joins and x < 2)
                                    or ("r" in joins and x >= 14)):
                px[x, y] = source[(x + 8) % 16, y]
    return out


class Renderer:
    """Keeps a 1x picture of the terrain, repainting only cells that changed."""

    def __init__(self, art):
        self.art = art
        self.reset()

    def reset(self):
        self.size = None
        self.keys = {}
        self.base = self.sky = None

    def update(self, section):
        size = (section.width * TILE, section.rows * TILE)
        if size != self.size:
            self.size, self.keys = size, {}
            self.sky = Image.new("RGBA", size)
            sky = self.art.sky
            for x in range(0, size[0], sky.width):  # anchored to the bottom
                y = size[1] - sky.height
                while y > -sky.height:
                    self.sky.paste(sky, (x, y))
                    y -= sky.height
            self.base = self.sky.copy()
        keys = tile_keys(section)
        for pos in set(keys) | set(self.keys):
            if keys.get(pos) != self.keys.get(pos):
                self._paint(pos, keys.get(pos))
        self.keys = keys

    def _paint(self, pos, key):
        x, y = pos[0] * TILE, pos[1] * TILE
        self.base.paste(self.sky.crop((x, y, x + TILE, y + TILE)), (x, y))
        if key is None:
            return
        if key[0] == "dirt":
            self.base.alpha_composite(self.art.dirt(*key[1:]), (x, y))
            return
        tile = self.art.tiles[key[1]]
        if key[0] == "grass":
            tile = joined(tile, key[3])
        self.base.alpha_composite(tile, (x, y))
        if key[0] == "grass":
            draw = ImageDraw.Draw(self.base)
            end = TILE - 1
            lines = {"t": (x, y, x + end, y), "b": (x, y + end, x + end, y + end),
                     "l": (x, y, x, y + end), "r": (x + end, y, x + end, y + end)}
            for edge in key[2]:
                draw.line(lines[edge], fill=(0, 0, 0, 255))

    def view(self, section, left, top, right, bottom):
        """The map between these 1x pixel coordinates, with everything on it."""
        image = self.base.crop((left, top, right, bottom))
        first = max(0, left // TILE - 2)
        last = min(section.width, right // TILE + 3)
        for gy, line in enumerate(section.grid):
            floor = (gy + 1) * TILE
            if floor <= top - TILE or floor - 2 * TILE >= bottom:
                continue
            for gx in range(first, last):
                sprite = self.art.sprites.get(line[gx])
                if sprite is None:
                    continue
                x = gx * TILE + (TILE - sprite.width) // 2 - left
                # coins float in the middle of their cell, the rest sit on it
                lift = (TILE - sprite.height) // 2 if line[gx] == "C" else 0
                y = floor - sprite.height - lift - top
                paste_clipped(image, sprite, x, y)
        return image


def paste_clipped(image, sprite, x, y):
    """alpha_composite that allows the sprite to hang off any edge."""
    sx, sy = max(0, -x), max(0, -y)
    if sx >= sprite.width or sy >= sprite.height:
        return
    if x >= image.width or y >= image.height:
        return
    image.alpha_composite(sprite, (max(0, x), max(0, y)), (sx, sy))


# -------------------------------------------------------------------- editor

class Editor:
    def __init__(self, root, path=None):
        self.root = root
        self.art = Art()
        self.renderer = Renderer(self.art)
        self.brush = tk.StringVar(value="X")
        self.before_eraser = "X"
        self.brush.trace_add("write", self.brush_changed)
        self.zoom = tk.IntVar(value=2)
        self.show_grid = tk.BooleanVar(value=True)
        self.spot = tk.StringVar(value="0")
        self.status = tk.StringVar()
        self.icons = {}
        self.stroke = None       # (button, start cell, last cell)
        self.pending = False
        self.photo = None
        self._build_ui()
        self._bind_keys()
        self.describe(None)
        if path and Path(path).exists():
            self.open_file(Path(path))
        else:
            self.new_level(Path(path) if path else None)

    # ---- layout

    def _build_ui(self):
        root = self.root
        root.geometry("1280x820")
        root.minsize(900, 720)  # keeps the whole palette visible
        top = ttk.Frame(root, padding=4)
        top.pack(side="top", fill="x")
        for text, command in (("New", self.new_level), ("Open", self.ask_open),
                              ("Save", self.save), ("Save as", self.save_as),
                              ("Undo", self.undo), ("Redo", self.redo),
                              ("Check", self.check)):
            ttk.Button(top, text=text, command=command, width=8).pack(side="left")
        ttk.Label(top, text="  Map:").pack(side="left")
        self.map_box = ttk.Combobox(top, state="readonly", width=12)
        self.map_box.pack(side="left")
        self.map_box.bind("<<ComboboxSelected>>",
                          lambda e: self.show_section(self.map_box.current()))
        ttk.Button(top, text="+ Sublevel", command=self.add_sublevel).pack(side="left")
        ttk.Button(top, text="Delete", command=self.delete_sublevel).pack(side="left")
        ttk.Button(top, text="Resize", command=self.ask_resize).pack(side="left", padx=(8, 0))
        ttk.Label(top, text="  Zoom:").pack(side="left")
        zoom = ttk.Combobox(top, state="readonly", width=3, values=ZOOMS,
                            textvariable=self.zoom)
        zoom.pack(side="left")
        zoom.bind("<<ComboboxSelected>>", lambda e: self.refresh())
        ttk.Checkbutton(top, text="Grid", variable=self.show_grid,
                        command=self.refresh).pack(side="left", padx=8)

        ttk.Label(root, textvariable=self.status, padding=4,
                  relief="sunken").pack(side="bottom", fill="x")

        side = ttk.Frame(root, padding=4)
        side.pack(side="left", fill="y")
        for title, items in PALETTE:
            ttk.Label(side, text=title).pack(anchor="w", pady=(6, 0))
            group = ttk.Frame(side)
            group.pack(fill="x")
            for i, (ch, name) in enumerate(items):
                tk.Radiobutton(
                    group, text=name.replace(" (", "\n("), image=self.icon(ch),
                    compound="top", variable=self.brush, value=ch,
                    indicatoron=False, width=78, height=58, wraplength=78,
                    selectcolor="#9ecbff").grid(row=i // 4, column=i % 4,
                                                sticky="nsew")
        row = ttk.Frame(side)
        row.pack(fill="x", pady=(6, 0))
        tk.Radiobutton(row, text="Bean spot", variable=self.brush,
                       value="spot", indicatoron=False, padx=4,
                       selectcolor="#9ecbff").pack(side="left")
        ttk.Combobox(row, state="readonly", width=3, textvariable=self.spot,
                     values=list("0123456789")).pack(side="left", padx=4)

        middle = ttk.Frame(root)
        middle.pack(side="left", fill="both", expand=True)
        middle.rowconfigure(0, weight=1)
        middle.columnconfigure(0, weight=1)
        self.canvas = tk.Canvas(middle, bg="#333333", highlightthickness=0,
                                xscrollincrement=1, yscrollincrement=1,
                                cursor="crosshair")
        self.canvas.grid(row=0, column=0, sticky="nsew")
        xbar = ttk.Scrollbar(middle, orient="horizontal", command=self.scroll_x)
        ybar = ttk.Scrollbar(middle, orient="vertical", command=self.scroll_y)
        xbar.grid(row=1, column=0, sticky="ew")
        ybar.grid(row=0, column=1, sticky="ns")
        self.canvas.configure(xscrollcommand=xbar.set, yscrollcommand=ybar.set)

    def icon(self, ch):
        image = Image.new("RGBA", (TILE, TILE), (0, 0, 0, 0))
        tiles = self.art.tiles
        if ch == "X":
            image = tiles["grass"]
        elif ch == "H":
            image = tiles["dirt"]
        elif ch == "Y":
            image = tiles["yellow"]
        elif ch in tuple(ARROWS):
            image = tiles["arrow" + ch]
        elif ch in self.art.sprites:
            sprite = self.art.sprites[ch]
            side = max(TILE, sprite.width, sprite.height)
            image = Image.new("RGBA", (side, side), (0, 0, 0, 0))
            image.alpha_composite(sprite, ((side - sprite.width) // 2,
                                           (side - sprite.height) // 2))
        self.icons[ch] = ImageTk.PhotoImage(image.resize((24, 24), Image.NEAREST))
        return self.icons[ch]

    def _bind_keys(self):
        c = self.canvas
        for button in (1, 3):
            c.bind(f"<ButtonPress-{button}>", lambda e, b=button: self.press(e, b))
            c.bind(f"<B{button}-Motion>", self.drag)
            c.bind(f"<ButtonRelease-{button}>", self.release)
        c.bind("<ButtonPress-2>", self.pick)
        c.bind("<Motion>", self.hover)
        c.bind("<Configure>", lambda e: self.refresh())
        c.bind("<MouseWheel>", lambda e: self.scroll_y("scroll", -e.delta // 2, "units"))
        c.bind("<Shift-MouseWheel>", lambda e: self.scroll_x("scroll", -e.delta // 2, "units"))
        c.bind("<Control-MouseWheel>", self.wheel_zoom)
        for key, command in (("n", self.new_level), ("o", self.ask_open),
                             ("s", self.save), ("z", self.undo), ("y", self.redo)):
            self.root.bind(f"<Control-{key}>", lambda e, f=command: f())
        # number keys pick a block; 1 swaps between the two grasses
        for key, choices in (("1", ("X", "Y")), ("2", ("H",)), ("3", ("B",)),
                             ("4", ("M",)), ("5", ("C",))):
            self.root.bind(f"<KeyPress-{key}>",
                           lambda e, c=choices: self.choose(*c))
        for key in ("e", "E"):
            self.root.bind(f"<KeyPress-{key}>", lambda e: self.toggle_eraser())
        self.root.protocol("WM_DELETE_WINDOW", self.quit)

    # ---- brush

    def choose(self, first, second=None):
        again = second and self.brush.get() == first
        self.brush.set(second if again else first)

    def toggle_eraser(self):
        if self.brush.get() == ".":
            self.brush.set(self.before_eraser)
        else:
            self.brush.set(".")

    def brush_changed(self, *_):
        """Remember what to go back to, however the brush was picked."""
        if self.brush.get() != ".":
            self.before_eraser = self.brush.get()

    # ---- document

    @property
    def section(self):
        return self.sections[self.current]

    def snapshot(self):
        return tuple((s.number, tuple("".join(r) for r in s.grid), tuple(s.comments))
                     for s in self.sections)

    def restore(self, state):
        self.sections = [Section(n, [list(r) for r in grid], comments)
                         for n, grid, comments in state]
        self.show_section(min(self.current, len(self.sections) - 1), keep_view=True)

    def remember(self):
        """Call before changing anything."""
        self.undo_stack.append(self.snapshot())
        del self.undo_stack[:-UNDO_LIMIT]
        self.redo_stack.clear()

    def undo(self):
        if self.undo_stack and not self.stroke:
            self.redo_stack.append(self.snapshot())
            self.restore(self.undo_stack.pop())

    def redo(self):
        if self.redo_stack and not self.stroke:
            self.undo_stack.append(self.snapshot())
            self.restore(self.redo_stack.pop())

    @property
    def dirty(self):
        return self.snapshot() != self.saved

    def set_document(self, sections, path, saved):
        self.sections, self.path = sections, path
        self.undo_stack, self.redo_stack = [], []
        self.saved = self.snapshot() if saved else None
        self.show_section(0)

    def may_discard(self):
        if not hasattr(self, "sections") or not self.dirty:
            return True
        answer = messagebox.askyesnocancel(
            "Unsaved changes", f"Save changes to {self.path.name} first?")
        if answer is None:
            return False
        return self.save() if answer else True

    def new_level(self, path=None):
        if not self.may_discard():
            return
        if path is None:
            number = 1
            while (levels.LEVELS_DIR / f"level{number}.txt").exists():
                number += 1
            path = levels.LEVELS_DIR / f"level{number}.txt"
        main = Section.blank(None, NEW_WIDTH, NEW_HEIGHT)
        main.comments = [f"# Super Flashy Birds - {path.stem}",
                         "# See levels/README.md for how each symbol behaves."]
        self.set_document([main], path, saved=False)

    def ask_open(self):
        if not self.may_discard():
            return
        name = filedialog.askopenfilename(
            initialdir=levels.LEVELS_DIR, filetypes=[("Level files", "*.txt")])
        if name:
            self.open_file(Path(name))

    def open_file(self, path):
        try:
            text = path.read_text(encoding="utf-8")
        except (OSError, UnicodeDecodeError) as err:
            messagebox.showerror("Could not open", str(err))
            if not hasattr(self, "sections"):
                self.new_level()
            return
        self.set_document(read_level(text), path.resolve(), saved=True)

    def save(self):
        try:
            text = write_level(self.sections)
            self.path.parent.mkdir(parents=True, exist_ok=True)
            self.path.write_text(text, encoding="utf-8")
        except OSError as err:
            messagebox.showerror("Could not save", str(err))
            return False
        self.saved = self.snapshot()
        self.update_title()
        error, warnings = check_level(text)
        if error:
            messagebox.showwarning(
                "Saved, but the game will not build",
                f"{self.path.name} was saved, but it has a problem:\n\n{error}")
        note = f", {len(warnings)} warning(s): press Check" if warnings else ""
        self.status.set(f"Saved {self.path}{note}")
        return True

    def save_as(self):
        name = filedialog.asksaveasfilename(
            initialdir=self.path.parent, initialfile=self.path.name,
            defaultextension=".txt", filetypes=[("Level files", "*.txt")])
        if name:
            self.path = Path(name)
            self.save()

    def check(self):
        error, warnings = check_level(write_level(self.sections))
        if error:
            messagebox.showerror("Level problem", error)
        elif warnings:
            messagebox.showwarning("Level warnings", "\n\n".join(warnings))
        else:
            messagebox.showinfo("Level check", "No problems found.")

    def quit(self):
        if self.may_discard():
            self.root.destroy()

    # ---- sublevels and size

    def show_section(self, index, keep_view=False):
        self.current = index
        self.stroke = None
        names = ["Main map" if s.number is None else f"Sublevel {s.number}"
                 for s in self.sections]
        self.map_box["values"] = names
        self.map_box.current(index)
        self.renderer.reset()
        self.refresh()
        if not keep_view:  # start at the bottom left, where the ground is
            self.root.update_idletasks()
            self.refresh_now()
            self.canvas.xview_moveto(0)
            self.canvas.yview_moveto(1)
            self.refresh()

    def add_sublevel(self):
        used = {s.number for s in self.sections}
        free = next((n for n in range(10) if n not in used), None)
        if free is None:
            messagebox.showinfo("Sublevels", "Sublevels 0-9 all exist already.")
            return
        number = simpledialog.askinteger(
            "New sublevel", "Sublevel number (0-9):", initialvalue=free,
            minvalue=0, maxvalue=9, parent=self.root)
        if number is None:
            return
        if number in used:
            messagebox.showinfo("Sublevels", f"Sublevel {number} already exists.")
            return
        self.remember()
        section = Section.blank(number, 30, NEW_HEIGHT)
        section.comments = [f"# Sublevel {number}"]
        self.sections.append(section)
        self.show_section(len(self.sections) - 1)

    def delete_sublevel(self):
        if self.section.number is None:
            messagebox.showinfo("Delete", "The main map can't be deleted.")
        elif messagebox.askyesno("Delete", f"Delete sublevel {self.section.number}?"):
            self.remember()
            del self.sections[self.current]
            self.show_section(0)

    def ask_resize(self):
        s = self.section
        width = simpledialog.askinteger(
            "Resize", "Width in tiles (added or removed on the right):",
            initialvalue=s.width, minvalue=levels.MIN_WIDTH, maxvalue=2000,
            parent=self.root)
        if width is None:
            return
        height = simpledialog.askinteger(
            "Resize", f"Height in tiles, up to {levels.MAX_ROWS} "
                      f"(added or removed at the top):",
            initialvalue=s.rows, minvalue=1, maxvalue=levels.MAX_ROWS,
            parent=self.root)
        if height is None or (width, height) == (s.width, s.rows):
            return
        cut = s.grid[:max(0, s.rows - height)]
        kept = s.grid[max(0, s.rows - height):]
        lost = any(ch != "." for row in cut for ch in row) or any(
            ch != "." for row in kept for ch in row[width:])
        if lost and not messagebox.askyesno(
                "Resize", "This removes blocks that are outside the new size. Continue?"):
            return
        self.remember()
        grid = [(row + ["."] * width)[:width] for row in kept]
        s.grid = [["."] * width for _ in range(height - len(grid))] + grid
        self.refresh()

    # ---- view

    def update_title(self):
        star = "*" if self.dirty else ""
        self.root.title(f"{star}{self.path.name} - Flashy Bird level editor")

    def refresh(self):
        if not self.pending and hasattr(self, "sections"):
            self.pending = True
            self.root.after_idle(self.refresh_now)

    def refresh_now(self):
        self.pending = False
        s, zoom, canvas = self.section, self.zoom.get(), self.canvas
        size = TILE * zoom
        canvas.configure(scrollregion=(0, 0, s.width * size, s.rows * size))
        self.renderer.update(s)
        left = max(0, int(canvas.canvasx(0)))
        top = max(0, int(canvas.canvasy(0)))
        x0, y0 = left // zoom, top // zoom
        x1 = min(s.width * TILE, (left + canvas.winfo_width()) // zoom + 1)
        y1 = min(s.rows * TILE, (top + canvas.winfo_height()) // zoom + 1)
        canvas.delete("all")
        if x1 <= x0 or y1 <= y0:
            return
        view = self.renderer.view(s, x0, y0, x1, y1)
        view = view.resize((view.width * zoom, view.height * zoom), Image.NEAREST)
        self.photo = ImageTk.PhotoImage(view)
        canvas.create_image(x0 * zoom, y0 * zoom, image=self.photo, anchor="nw")
        if self.show_grid.get():
            for x in range(x0 // TILE * size, x1 * zoom + 1, size):
                canvas.create_line(x, y0 * zoom, x, y1 * zoom, fill="#4a6a80", stipple="gray50")
            for y in range(y0 // TILE * size, y1 * zoom + 1, size):
                canvas.create_line(x0 * zoom, y, x1 * zoom, y, fill="#4a6a80", stipple="gray50")
        self.update_title()

    def scroll_x(self, *args):
        self.canvas.xview(*args)
        self.refresh()

    def scroll_y(self, *args):
        self.canvas.yview(*args)
        self.refresh()

    def wheel_zoom(self, event):
        index = ZOOMS.index(self.zoom.get()) + (1 if event.delta > 0 else -1)
        if not 0 <= index < len(ZOOMS):
            return
        old, new = self.zoom.get(), ZOOMS[index]
        # keep the spot under the mouse where it is
        x = self.canvas.canvasx(event.x) / old
        y = self.canvas.canvasy(event.y) / old
        self.zoom.set(new)
        w, h = self.section.width * TILE * new, self.section.rows * TILE * new
        self.canvas.configure(scrollregion=(0, 0, w, h))
        self.canvas.xview_moveto(max(0, x * new - event.x) / w)
        self.canvas.yview_moveto(max(0, y * new - event.y) / h)
        self.refresh()

    # ---- painting

    def cell_at(self, event, clamp=False):
        size = TILE * self.zoom.get()
        x = int(self.canvas.canvasx(event.x) // size)
        y = int(self.canvas.canvasy(event.y) // size)
        s = self.section
        if clamp:
            return min(max(x, 0), s.width - 1), min(max(y, 0), s.rows - 1)
        return (x, y) if 0 <= x < s.width and 0 <= y < s.rows else None

    def brush_for(self, button):
        if button == 3:
            return "."
        return self.spot.get() if self.brush.get() == "spot" else self.brush.get()

    def put(self, cell, ch):
        x, y = cell
        grid = self.section.grid
        if ch == "S":  # there is only one start per map
            for row in grid:
                row[:] = ["." if c == "S" else c for c in row]
        grid[y][x] = ch

    def press(self, event, button):
        cell = self.cell_at(event)
        if cell is None or self.stroke:
            return
        self.remember()
        rect = bool(event.state & 0x0001)  # Shift held: fill a rectangle
        self.stroke = (button, cell, cell, rect)
        if not rect:
            self.put(cell, self.brush_for(button))
            self.refresh()

    def drag(self, event):
        if not self.stroke:
            return
        button, start, last, rect = self.stroke
        cell = self.cell_at(event, clamp=True)
        self.describe(cell)
        if cell == last:
            return
        self.stroke = (button, start, cell, rect)
        if rect:
            size = TILE * self.zoom.get()
            self.refresh_now()
            self.canvas.create_rectangle(
                min(start[0], cell[0]) * size, min(start[1], cell[1]) * size,
                (max(start[0], cell[0]) + 1) * size,
                (max(start[1], cell[1]) + 1) * size, outline="white", width=2)
            return
        steps = max(abs(cell[0] - last[0]), abs(cell[1] - last[1]))
        for i in range(1, steps + 1):  # no gaps when the mouse moves fast
            self.put((round(last[0] + (cell[0] - last[0]) * i / steps),
                      round(last[1] + (cell[1] - last[1]) * i / steps)),
                     self.brush_for(button))
        self.refresh()

    def release(self, event):
        if not self.stroke:
            return
        button, start, last, rect = self.stroke
        self.stroke = None
        if rect:
            ch = self.brush_for(button)
            for y in range(min(start[1], last[1]), max(start[1], last[1]) + 1):
                for x in range(min(start[0], last[0]), max(start[0], last[0]) + 1):
                    self.put((x, y), ch)
        if self.undo_stack and self.undo_stack[-1] == self.snapshot():
            self.undo_stack.pop()  # nothing changed
        self.refresh()

    def pick(self, event):
        cell = self.cell_at(event)
        if cell is None:
            return
        ch = self.section.grid[cell[1]][cell[0]]
        if ch.isdigit():
            self.spot.set(ch)
            self.brush.set("spot")
        elif ch in NAMES:
            self.brush.set(ch)

    def hover(self, event):
        self.describe(self.cell_at(event))

    def describe(self, cell):
        help_text = ("Left: place   Right: erase   Shift+drag: fill area   "
                     "Middle: pick   Wheel: scroll (Shift: sideways, Ctrl: zoom)   "
                     "E: eraser   1-5: grass, dirt, box, mystery, coin")
        if cell is None:
            self.status.set(help_text)
            return
        ch = self.section.grid[cell[1]][cell[0]]
        name = "air" if ch == "." else NAMES.get(ch, f"unknown {ch!r}")
        self.status.set(f"Column {cell[0] + 1}, row {self.section.rows - cell[1]} "
                        f"from the bottom: {name}      |      {help_text}")


def main():
    root = tk.Tk()
    if ICON.exists():
        root.iconbitmap(default=str(ICON))
    path = sys.argv[1] if len(sys.argv) > 1 else None
    if path is None and (levels.LEVELS_DIR / "level1.txt").exists():
        path = levels.LEVELS_DIR / "level1.txt"
    try:
        Editor(root, Path(path).resolve() if path else None)
    except Exception as err:  # the desktop shortcut has no console to show it
        messagebox.showerror("Level editor could not start", str(err))
        raise
    root.mainloop()


if __name__ == "__main__":
    main()
