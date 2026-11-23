"""sprutm.ico for SprutmViewer.exe: a dark rounded square with a small screen and a CVBS 'signal' line."""
from PIL import Image, ImageDraw

S = 256
im = Image.new("RGBA", (S, S), (0, 0, 0, 0))
d = ImageDraw.Draw(im)
d.rounded_rectangle((8, 8, S - 8, S - 8), radius=48, fill=(22, 24, 28, 255))
d.rounded_rectangle((40, 56, S - 40, S - 72), radius=18, fill=(10, 12, 14, 255), outline=(79, 163, 255, 255), width=10)
# one video line: sync pulse, burst, steps of a picture
pts = [(60, 150), (78, 150), (78, 168), (92, 168), (92, 150), (108, 150), (108, 128), (132, 128), (132, 108),
       (160, 108), (160, 88), (196, 88), (196, 150)]
d.line(pts, fill=(232, 234, 237, 255), width=8, joint="curve")
d.rounded_rectangle((96, S - 56, S - 96, S - 40), radius=8, fill=(79, 163, 255, 255))
im.save("sprutm.ico", sizes=[(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)])
print("sprutm.ico")
