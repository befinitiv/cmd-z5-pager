# Level maps of the jump and run game (src/screens/game.c): 8 rows of tiles
# below the score line, row 0 = top. Run from sw/:
#   python3 tools/game/level.py > src/screens/game_level.h
#
# Tiles: '#' ground (sand under water), 'B' brick, '?' coin block, 'M' block
# with a grow item, '=' hard block (coral under water), '[]' / '{}' pipe top /
# body, 'o' coin, '|' / 'F' flag pole / flag (goal), 'X' / 'Y' exit pipe
# opening top / bottom (goal under water), 'x' / 'y' exit pipe body top /
# bottom. Decoration: '()' cloud, '<>' bush, 'w' seaweed.
# Spawns: 'g' walker, 'f' fish, 'j' jelly.


class Level:
    def __init__(self, w):
        self.w = w
        self.g = [[' '] * w for _ in range(8)]

    def put(self, x, y, s):
        for i, c in enumerate(s):
            self.g[y][x + i] = c

    def ground(self, x0, x1):
        for x in range(x0, x1):
            self.g[6][x] = '#'
            self.g[7][x] = '#'

    def gap(self, x0, x1):
        for x in range(x0, x1):
            self.g[6][x] = ' '
            self.g[7][x] = ' '

    def pipe(self, x, h):
        top = 6 - h
        self.put(x, top, '[]')
        for y in range(top + 1, 6):
            self.put(x, y, '{}')

    def column(self, x, h):  # hard blocks standing on the ground
        for y in range(6 - h, 6):
            self.g[y][x] = '='

    def hanging(self, x, h):  # hard blocks hanging from the top
        for y in range(0, h):
            self.g[y][x] = '='

    def rows(self):
        for row in self.g:
            assert len(row) == self.w
        return [''.join(row) for row in self.g]


def level_1():
    """1-1: overground, pits, pipes, walkers, a flag at the end."""
    lv = Level(200)
    lv.ground(0, lv.w)
    for a, b in [(54, 56), (79, 82), (96, 98), (125, 136), (156, 158)]:
        lv.gap(a, b)

    # Decoration: clouds and bushes.
    for x, y in [(3, 1), (19, 0), (33, 2), (51, 1), (70, 0), (88, 2), (107, 1), (126, 0),
                 (145, 2), (166, 1), (190, 1)]:
        lv.put(x, y, '()')
    for x in [6, 23, 43, 61, 89, 113, 142, 167, 179]:
        lv.put(x, 5, '<>')

    # Start: blocks, first mushroom, walkers, pipes.
    lv.put(10, 3, '?')
    lv.put(14, 3, 'BMB?B')
    lv.put(16, 0, '?')
    lv.put(20, 5, 'g')
    lv.pipe(26, 2)
    lv.pipe(35, 3)
    lv.put(39, 5, 'g'); lv.put(41, 5, 'g')
    lv.pipe(46, 3)
    lv.put(52, 2, 'ooo')  # coins over the first pit
    # Brick bridge with coins above.
    lv.put(59, 3, 'BBB?BBB')
    lv.put(60, 1, 'ooooo')
    lv.put(63, 5, 'g'); lv.put(66, 5, 'g')
    lv.put(70, 3, '?'); lv.put(73, 3, '?'); lv.put(73, 0, 'M'); lv.put(76, 3, '?')
    lv.put(79, 2, 'oooo')
    # Staircase up and down around a pit.
    for i in range(4):
        lv.column(91 + i, i + 1)
    lv.column(95, 4)
    for i in range(4):
        lv.column(98 + i, 4 - i)
    lv.put(84, 5, 'g'); lv.put(87, 5, 'g')
    lv.put(96, 1, 'oo')
    lv.put(108, 3, 'B?BB'); lv.put(110, 0, 'oo')
    lv.put(115, 5, 'g'); lv.put(117, 5, 'g'); lv.put(119, 5, 'g')
    # Long pit with pillars.
    lv.column(128, 1); lv.column(129, 1)
    lv.column(132, 2); lv.column(133, 2)
    lv.put(126, 3, 'oo'); lv.put(130, 2, 'oo'); lv.put(134, 2, 'oo')
    lv.put(141, 3, 'BMB'); lv.put(146, 5, 'g'); lv.put(149, 5, 'g')
    lv.pipe(152, 2)
    lv.put(156, 3, 'oo')
    lv.put(160, 5, 'g'); lv.put(163, 5, 'g'); lv.put(165, 3, '?B?')
    # Final staircase and flag.
    for i in range(5):
        lv.column(170 + i, i + 1)
    lv.column(175, 5)
    lv.put(183, 5, '=')
    for y in range(0, 5):
        lv.g[y][183] = '|'
    lv.g[0][183] = 'F'
    return lv


def level_2():
    """1-2: under water, coral passages, fish and jellies, an exit pipe."""
    lv = Level(176)
    lv.ground(0, lv.w)

    # Seaweed on the sea floor.
    for x in [3, 9, 17, 24, 31, 52, 58, 66, 79, 88, 101, 109, 122, 131, 143, 150]:
        lv.put(x, 5, 'w')

    # Start: a block with a grow item and some coins.
    lv.put(12, 2, '?M?')
    lv.put(20, 1, 'ooo')

    # Coral pillars from the floor and from the top.
    lv.column(28, 2); lv.column(29, 3)
    lv.hanging(35, 2); lv.hanging(36, 3)
    lv.put(32, 4, 'oo')
    lv.put(26, 3, 'f'); lv.put(40, 2, 'f')

    # First passage: two tiles high, coins inside.
    for x in range(44, 52):
        lv.hanging(x, 3)
        lv.column(x, 1)
    lv.put(45, 3, 'oooooo')
    lv.put(55, 4, 'f'); lv.put(60, 1, 'j')

    # Open water with a coin ring and blocks.
    lv.put(64, 1, 'oo'); lv.put(63, 2, 'o'); lv.put(66, 2, 'o'); lv.put(64, 3, 'oo')
    lv.put(70, 3, 'B?B')
    lv.put(74, 2, 'f'); lv.put(78, 4, 'f'); lv.put(83, 1, 'j')

    # Coral wall with a gap at the top, then one with a gap at the bottom.
    for x in (86, 87):
        lv.column(x, 4)
    lv.put(86, 0, 'oo')
    for x in (93, 94):
        lv.hanging(x, 4)
    lv.put(93, 5, 'oo')
    lv.put(97, 3, 'f'); lv.put(99, 1, 'j')

    # Zigzag: pillars from the floor and the top in turn.
    for i, x in enumerate(range(104, 128, 4)):
        if i % 2 == 0:
            lv.column(x, 3); lv.column(x + 1, 3)
            lv.put(x, 1, 'oo')
        else:
            lv.hanging(x, 3); lv.hanging(x + 1, 3)
            lv.put(x, 4, 'oo')
    lv.put(107, 2, 'f'); lv.put(115, 3, 'f'); lv.put(123, 2, 'f'); lv.put(118, 0, 'j')

    # Second passage, lower, with a block above the entrance.
    lv.put(131, 1, '?M')
    for x in range(134, 144):
        lv.hanging(x, 4)
    lv.put(135, 4, 'oooooooo')
    lv.put(147, 3, 'f'); lv.put(150, 1, 'j'); lv.put(153, 4, 'f')

    # Exit: a pipe opening in a coral wall.
    for x in range(160, lv.w):
        lv.hanging(x, 4)
    lv.put(160, 4, 'X' + 'x' * (lv.w - 161))
    lv.put(160, 5, 'Y' + 'y' * (lv.w - 161))
    return lv


levels = [('LEVEL1', level_1()), ('LEVEL2', level_2())]

print('/* Generated by tools/game/level.py, do not edit. */')
print()
print('#pragma once')
for name, lv in levels:
    print()
    print('#define %s_W %d' % (name, lv.w))
    print('static const char %s_map[8][%s_W + 1] = {' % (name.lower(), name))
    for row in lv.rows():
        print('\t"' + row + '",')
    print('};')
