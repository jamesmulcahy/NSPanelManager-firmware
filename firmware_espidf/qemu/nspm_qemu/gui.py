"""Data, no code: the TFT components tests touch, by their names in gui.HMI.

The display looks names up in the HMI, so tests never hard-code a component id: an id that moves
in the HMI is picked up, and a name the HMI no longer has fails the touch. Add a component here,
once, rather than spelling its name in a test.
"""

# home
HOME_CEILING = "h_ceiling"  # ceiling lights on/off
HOME_ROOM = "room"  # the room's name; touching it opens the room's entities page
HOME_NEXT_ROOM = "b_nextroom"
HOME_BRIGHTNESS = "s_brightness"

# items4, the 4-slot entities page
ITEMS4_SLOT1 = "i1_button"
