"""lib/Pages/GUI_data.hpp against the TFT's HMI: every page, component, id and picture the
firmware hard-codes must exist on the display. No QEMU needed.

A mismatch here is a button that never responds, or a label that is never drawn. The display
answers 0x1A, which the firmware logs only at DEBUG, so on a panel it goes unnoticed.
"""

import os
import re
import textwrap
from dataclasses import dataclass
from typing import Dict, List, Optional, Tuple

import pytest

from nspm_qemu import hmi as nspm_hmi
from nspm_qemu.rig import PROJECT_DIR

GUI_DATA = os.path.join(PROJECT_DIR, "lib", "Pages", "GUI_data.hpp")

# Classes without a page_name of their own, and the pages they serve.
SHARED = {"GUI_ITEMS_PAGE_COMMON": ["items4", "items8", "items12"]}

# Mismatches already known. Each must still fail; when one is fixed, the test says to drop it.
KNOWN: Dict[str, str] = {
    # Unused: the firmware never reads it. Both HMIs put lights at 9.
    "GUI_LIGHT_CONTROL_PAGE.page_id": "stale, unused",
    # Unused: the thermostat page reports through 0x99 strings (printh 99 / prints), not ids.
    "GUI_THERMOSTAT_CONTROL_PAGE.back_button_id": "unused",
    "GUI_THERMOSTAT_CONTROL_PAGE.next_button_id": "unused",
    "GUI_THERMOSTAT_CONTROL_PAGE.previous_button_id": "unused",
    "GUI_THERMOSTAT_CONTROL_PAGE.next_mode_button_id": "unused",
    "GUI_THERMOSTAT_CONTROL_PAGE.previous_mode_button_id": "unused",
    # Added by #55. Its page is in NSPanelManager#419's HMI, but that HMI put it in screensaver2's
    # slot and dropped screensaver2, which the firmware still uses, so nspanelmanager.ref stays
    # before it (NSPManager/NSPanelManager#427).
    "GUI_COVER_CONTROL_PAGE.page_name": "cover page not in the pinned HMI",
    # Added by #27. Its page is added to the HMIs by NSPanelManager#408, not merged yet.
    "GUI_MEDIA_PLAYER_CONTROL_PAGE.page_name": "mediaplayer page not in the pinned HMI",
}


@dataclass
class Constant:
    cls: str
    name: str
    value: object  # str, int, or a list of str


def _strip_comments(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def parse(text: str) -> Tuple[Dict[str, List[Constant]], List[Tuple[str, str, int, str]]]:
    """({class: constants}, [(class, id constant, id, component name)] from struct initialisers)."""
    text = _strip_comments(text)
    classes: Dict[str, List[Constant]] = {}
    pairs = []
    # A class ends at a "};" in column 0; arrays inside it close indented.
    for m in re.finditer(r"\b(?:class|struct)\s+(\w+)\s*\{(.*?)\n\};", text, re.S):
        cls, body = m.group(1), m.group(2)
        constants = []
        for c in re.finditer(r"static\s+inline\s+constexpr\s+[\w\s]*?\*?\s*(\w+)\s*(\[\])?\s*=\s*(.*?);", body, re.S):
            name, is_array, value = c.group(1), c.group(2), c.group(3).strip()
            if is_array:
                strings = re.findall(r'"([^"]*)"', value)
                if strings and "." not in value.replace('"', ""):
                    constants.append(Constant(cls, name, strings))
                for p in re.finditer(r'\.(\w+)_id\s*=\s*(\d+)\s*,\s*\.(\w+)_name\s*=\s*"([^"]*)"', value):
                    if p.group(1) == p.group(3):
                        pairs.append((cls, p.group(1) + "_id", int(p.group(2)), p.group(4)))
            elif value.startswith('"'):
                constants.append(Constant(cls, name, value.strip('"')))
            elif re.fullmatch(r"\d+", value):
                constants.append(Constant(cls, name, int(value)))
        classes[cls] = constants
    return classes, pairs


def _pages_of(cls: str, constants: List[Constant]) -> List[str]:
    for c in constants:
        if c.name == "page_name":
            return [c.value]
    return SHARED.get(cls, [])


_TOUCH_FRAME = re.compile(r"printh\s+65\s+([0-9A-Fa-f]{2})\s+([0-9A-Fa-f]{2})\s+0?[01]\b")


def touch_frames(page):
    """(page byte, component id) of every touch event the page's own code fakes with printh.
    The items, lights and thermostat pages decode gestures on a touch-capture component and
    report them this way, for buttons that report nothing themselves."""
    found = set()
    for comp in page.components:
        for code in comp.events.values():
            for line in code:
                for m in _TOUCH_FRAME.finditer(line.split("//")[0]):
                    found.add((int(m.group(1), 16), int(m.group(2), 16)))
    return found


def _overlaps(a, b) -> bool:
    ax, ay, aw, ah = (a.attrs.get(k, 0) for k in ("x", "y", "w", "h"))
    bx, by, bw, bh = (b.attrs.get(k, 0) for k in ("x", "y", "w", "h"))
    return ax < bx + bw and bx < ax + aw and ay < by + bh and by < ay + ah


class Checker:
    def __init__(self, project):
        self.hmi = project
        self.problems: Dict[str, str] = {}

    def problem(self, key: str, message: str) -> None:
        self.problems[key] = message

    def component(self, ref: str, page_name: str):
        """A "page.comp" or bare "comp" reference: (page, component) or None."""
        if "." in ref:
            page_name, _, ref = ref.partition(".")
        page = self.hmi.find_page(page_name)
        return (page, page.find(ref)) if page is not None and page.find(ref) else None

    def check(self, classes, pairs) -> Dict[str, str]:
        self._classes = classes
        self._faked = {}
        for page in self.hmi.pages:
            frames = touch_frames(page)
            for page_byte, cid in sorted(frames):
                if page_byte != page.id:
                    self.problem("HMI %s: printh 65 %02X %02X" % (page.name, page_byte, cid),
                                 "the page's code reports touches as page %d, but it is page %d" % (page_byte, page.id))
            self._faked[page.name] = {cid for page_byte, cid in frames}
        for cls, constants in classes.items():
            pages = _pages_of(cls, constants)
            by_name = {c.name: c for c in constants}
            for page_name in pages:
                if self.hmi.find_page(page_name) is None:
                    self.problem("%s.page_name" % cls, "page %r is not in the HMI" % page_name)
                    continue
                for c in constants:
                    self._check_constant(cls, c, page_name, by_name)
        for cls, id_name, number, comp_name in pairs:
            for page_name in _pages_of(cls, classes.get(cls, [])):
                self._check_id("%s.%s(%s)" % (cls, id_name, comp_name), number, page_name, comp_name)
        return self.problems

    def _check_constant(self, cls, c: Constant, page_name: str, by_name) -> None:
        key = "%s.%s" % (cls, c.name) + (" on %s" % page_name if cls in SHARED else "")
        if c.name == "page_name" or c.name.endswith("_text"):
            return  # _text: what the firmware writes into a component, not a component's name
        if c.name.endswith("page_name"):
            if self.hmi.find_page(c.value) is None:
                self.problem(key, "page %r is not in the HMI" % c.value)
        elif isinstance(c.value, (str, list)):
            for ref in c.value if isinstance(c.value, list) else [c.value]:
                if self.component(ref, page_name) is None:
                    self.problem(key, "no component %r on %s" % (ref, page_name))
        elif c.name == "page_id":
            actual = self.hmi.page(page_name).id
            if actual != c.value:
                self.problem(key, "page %r has id %d in the HMI, not %d" % (page_name, actual, c.value))
        elif c.name.endswith("_id"):
            # An id with a name beside it (dimmer_slider_id, dimmer_slider_name) must be that
            # component, or a touch area over it.
            partner = by_name.get(c.name[:-3] + "_name")
            for shared, pages in SHARED.items():
                if partner is None and page_name in pages:
                    partner = {x.name: x for x in self._classes[shared]}.get(c.name[:-3] + "_name")
            self._check_id(key, c.value, page_name, partner.value if partner else None)
        elif "pic" in c.name and c.value >= self.hmi.picture_count:
            self.problem(key, "picture %d, but the HMI has %d" % (c.value, self.hmi.picture_count))

    def _check_id(self, key, number, page_name, target_name: Optional[str]) -> None:
        page = self.hmi.page(page_name)
        comp = page.by_id(number)
        if comp is None:
            self.problem(key, "no component with id %d on %s" % (number, page_name))
            return
        reports = comp.attrs.get("sendkey", 0) != 0 or number in self._faked.get(page_name, ())
        if target_name is None:
            if not reports:
                self.problem(key, "id %d on %s is %s (%s), which reports no touches"
                             % (number, page_name, comp.name, comp.kind))
            return
        found = self.component(target_name, page_name)
        if found is None:
            return  # reported by the name check
        target = found[1]
        if comp is target:
            return
        if reports and _overlaps(comp, target):
            return  # a touch area over the component, as the home page's hotspots are
        self.problem(key, "id %d on %s is %s, but the firmware means %s (id %d)"
                     % (number, page_name, comp.name, target.name, target.id))


@pytest.fixture(scope="module")
def problems():
    with open(GUI_DATA) as f:
        classes, pairs = parse(f.read())
    return Checker(nspm_hmi.load()).check(classes, pairs)


def test_gui_data_matches_the_hmi(problems):
    new = {k: v for k, v in problems.items() if k not in KNOWN}
    assert not new, "GUI_data.hpp disagrees with the HMI:\n" + "\n".join("  %s: %s" % kv for kv in sorted(new.items()))


@pytest.mark.parametrize("key", sorted(KNOWN) or [pytest.param(None, marks=pytest.mark.skip(reason="none known"))])
def test_known_mismatches_are_still_there(problems, key):
    assert key in problems, "%s is fixed: remove it from KNOWN" % key


def test_the_parser_reads_what_it_should():
    classes, pairs = parse(textwrap.dedent('''
        class GUI_X_PAGE {
        public:
          static inline constexpr const char *page_name = "home"; // comment
          static inline constexpr uint8_t button_id = 22;
          static inline constexpr const char *names[] = {"a", "b"};
          /* static inline constexpr uint8_t hidden_id = 1; */
          static inline constexpr GUI_ITEMS_PAGE_ITEM_DATA item_slots[] = {
              {.button_id = 5, .button_name = "i1_button", .label_id = 1, .label_name = "i1_label"},
          };
        };
    '''))
    values = {c.name: c.value for c in classes["GUI_X_PAGE"]}
    assert values == {"page_name": "home", "button_id": 22, "names": ["a", "b"]}
    assert pairs == [("GUI_X_PAGE", "button_id", 5, "i1_button"), ("GUI_X_PAGE", "label_id", 1, "i1_label")]
