# 分析 IDE 启动截图序列：内容什么时候出现、什么时候稳定。
#
#   python D:\Nodio\tests\ide_paint_timeline.py [目录] [间隔ms]
#
# 指标：
#   · 非背景像素数 —— 画面上「有东西」的程度（工具条/树/页签/代码）
#   · 与上一帧的差异 —— 画面还在变化的程度（稳定 = 0）
import os, glob, sys
from PIL import Image, ImageChops

d = sys.argv[1] if len(sys.argv) > 1 else r'D:\Nodio\build-x64\Release\paint_timeline'
interval = int(sys.argv[2]) if len(sys.argv) > 2 else 500

files = sorted(glob.glob(os.path.join(d, 't_*.png')))
if not files:
    print('NO-SHOTS')
    sys.exit(1)

prev = None
first_content = None
last_change = 0
print('%-4s %-8s %12s %10s  %s' % ('#', '时刻ms', '非背景像素', '与上帧差', '说明'))
for i, f in enumerate(files):
    im = Image.open(f).convert('RGB')
    g = im.convert('L')
    hist = g.histogram()
    total = im.size[0] * im.size[1]
    # 深色主题：背景接近 0x1e/0x20 一类的暗色；用「亮度>60」当“有内容”
    content = sum(hist[60:])
    dprev = 0
    if prev is not None:
        diff = ImageChops.difference(im, prev).convert('L')
        dprev = sum(diff.histogram()[12:])
    t = (i + 1) * interval
    note = ''
    if content > total * 0.005 and first_content is None:
        first_content = t
        note = '<= 首次出现内容'
    if dprev > total * 0.002:
        last_change = t
    print('%-4d %-8d %12d %10d  %s' % (i + 1, t, content, dprev, note))
    prev = im

print()
print('首次出现内容: %s ms' % (first_content if first_content else '未出现'))
print('最后一次明显变化: %s ms' % last_change)
print('→ 画面稳定约需 %s ms' % last_change)
