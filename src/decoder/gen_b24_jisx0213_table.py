# Generates the JIS X 0213:2004 kuten -> Unicode tables for libaribcaption from
# Python's own euc_jis_2004 codec, which is the JIS X 0213:2004 mapping.
# No table is copied from another project, so nothing here inherits a licence.
import io
import sys

PLANE1_BYTES = lambda ku, ten: bytes([0xA0 + ku, 0xA0 + ten])
PLANE2_BYTES = lambda ku, ten: bytes([0x8F, 0xA0 + ku, 0xA0 + ten])


def build(plane):
    """Returns (single[9036], pairs) - single holds one codepoint per cell (0 when
    undefined or when the cell needs two), pairs maps index -> (cp1, cp2)."""
    make = PLANE1_BYTES if plane == 1 else PLANE2_BYTES
    single = [0] * (94 * 94)
    pairs = {}
    for ku in range(1, 95):
        for ten in range(1, 95):
            try:
                s = make(ku, ten).decode('euc_jis_2004')
            except UnicodeDecodeError:
                continue
            idx = (ku - 1) * 94 + (ten - 1)
            if len(s) == 1:
                single[idx] = ord(s)
            elif len(s) == 2:
                pairs[idx] = (ord(s[0]), ord(s[1]))
            else:
                raise SystemExit('unexpected length %d at %d-%d' % (len(s), ku, ten))
    return single, pairs


def emit(out, name, values):
    out.write('inline constexpr uint32_t %s[94 * 94] = {\n' % name)
    for i in range(0, len(values), 8):
        chunk = values[i:i + 8]
        out.write('    ' + ' '.join('0x%04X,' % v for v in chunk) + '\n')
    out.write('};\n\n')


def main(path):
    p1, pairs1 = build(1)
    p2, pairs2 = build(2)
    sys.stderr.write('plane1 defined=%d pairs=%d\n' % (sum(1 for v in p1 if v), len(pairs1)))
    sys.stderr.write('plane2 defined=%d pairs=%d\n' % (sum(1 for v in p2 if v), len(pairs2)))

    # Sanity: the character this whole exercise is about.
    idx = (91 - 1) * 94 + (6 - 1)
    sys.stderr.write('plane1 kuten 91-6 = U+%04X\n' % p1[idx])

    out = io.open(path, 'w', encoding='utf-8', newline='\n')
    out.write('''/*
 * JIS X 0213:2004 kuten -> Unicode tables, indexed as (ku - 1) * 94 + (ten - 1).
 *
 * ARIB STD-B24 carries two "JIS compatible Kanji" graphic sets, final bytes
 * 0x39 and 0x3A, which are JIS X 0213:2004 plane 1 and plane 2. kKanjiTable
 * already holds plane 1 rows 1-84, so only the rest is read from here: rows
 * 85-94 of plane 1, which hold level 3/4 Kanji where the Kanji set has the ARIB
 * additional symbols instead, and plane 2, which kKanjiTable does not cover at
 * all. The rows that overlap are kept anyway so each table stands on its own.
 *
 * Cells JIS X 0213 leaves undefined are 0. So are the 25 cells in plane 1 rows
 * 4 and 5 that stand for a kana plus a combining semi-voiced mark, which is two
 * code points and does not fit one cell; all of them are in the rows that come
 * from kKanjiTable, so nothing reads them here.
 *
 * Generated from the JIS X 0213:2004 mapping - do not edit by hand.
 */

#ifndef ARIBCAPTION_B24_JISX0213_TABLE_HPP
#define ARIBCAPTION_B24_JISX0213_TABLE_HPP

#include <cstdint>

namespace aribcaption {

''')
    emit(out, 'kJISX0213Plane1Table', p1)
    emit(out, 'kJISX0213Plane2Table', p2)

    out.write('''}  // namespace aribcaption

#endif  // ARIBCAPTION_B24_JISX0213_TABLE_HPP
''')
    out.close()


main(sys.argv[1])
