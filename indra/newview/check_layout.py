import sys, xml.etree.ElementTree as ET

path = sys.argv[1]
tree = ET.parse(path)
root = tree.getroot()

panel = root.find('.//panel')
if panel is None:
    panel = root

prev_top = prev_bottom = 0
prev_left = prev_right = 0
rows = []
for el in panel:
    if not isinstance(el.tag, str):
        continue
    a = el.attrib
    h = int(float(a.get('height', 0)))
    w = int(float(a.get('width', 0)))

    if 'top' in a:
        top = int(float(a['top']))
    elif 'top_pad' in a:
        top = prev_bottom + int(float(a['top_pad']))
    elif 'top_delta' in a:
        top = prev_top + int(float(a['top_delta']))
    else:
        top = prev_bottom

    if 'left' in a:
        left = int(float(a['left']))
    elif 'left_pad' in a:
        left = prev_right + int(float(a['left_pad']))
    else:
        left = prev_left

    rows.append((top, top + h, left, left + w, a.get('name', el.tag), el.tag))
    prev_top, prev_bottom, prev_left, prev_right = top, top + h, left, left + w

panel_h = int(float(panel.attrib.get('height', 0)))
panel_w = int(float(panel.attrib.get('width', 0)))

# Overlap check between consecutive rows that share horizontal span
print('%-28s %-12s %6s %6s %6s %6s' % ('name', 'tag', 'top', 'bot', 'left', 'right'))
worst = 0
for i, r in enumerate(rows):
    flag = ''
    for j in range(i):
        p = rows[j]
        if r[0] < p[1] and r[1] > p[0] and r[2] < p[3] and r[3] > p[2]:
            flag = '  <== OVERLAPS %s' % p[4]
            break
    if r[3] > panel_w - 8:
        flag += '  <== TOO WIDE (panel %d)' % panel_w
    worst = max(worst, r[1])
    print('%-28s %-12s %6d %6d %6d %6d%s' % (r[4], r[5], r[0], r[1], r[2], r[3], flag))

print()
print('content bottom = %d, panel height = %d' % (worst, panel_h))
