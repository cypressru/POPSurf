/* The image producer/consumer pipeline, in the shape every sprite-sheet applet
 * of the period uses it: one sheet, cropped into tiles, drawn as a grid.
 *
 * The sheet is drawn here rather than fetched so this needs no artwork, and so
 * that a real JVM run and a runtime run start from byte-identical source
 * pixels - the point of this applet is that the two frames can be diffed, and
 * a decoder or a font difference would drown the thing being measured. For the
 * same reason nothing here draws a string or a line: only fillRect, whose
 * result is not open to interpretation.
 *
 * It covers the three edges a crop has: wholly inside the sheet, hanging off
 * it (which must come back transparent rather than black), and chained behind
 * another crop. MemoryImageSource and an RGBImageFilter subclass are the other
 * two producers an applet of the era reaches for.
 */
import java.applet.Applet;
import java.awt.Color;
import java.awt.Graphics;
import java.awt.Image;
import java.awt.image.ColorModel;
import java.awt.image.CropImageFilter;
import java.awt.image.DirectColorModel;
import java.awt.image.FilteredImageSource;
import java.awt.image.ImageFilter;
import java.awt.image.ImageProducer;
import java.awt.image.IndexColorModel;
import java.awt.image.MemoryImageSource;
import java.awt.image.PixelGrabber;
import java.awt.image.RGBImageFilter;

class Redden extends RGBImageFilter {
    Redden() {
        canFilterIndexColorModel = true;
    }

    public int filterRGB(int x, int y, int rgb) {
        int a = (rgb >>> 24) & 0xff;
        int r = (rgb >> 16) & 0xff;
        int g = (rgb >> 8) & 0xff;
        int b = rgb & 0xff;
        int v = (r + g + b) / 3;
        int hi = v + x * 3;
        if (hi > 255) hi = 255;
        return (a << 24) | (hi << 16) | (v << 8) | v;
    }
}

public class Tiles extends Applet {
    Image sheet, inside, over, chain, whole, mem, red, ident;
    Image indexed, packed, grabbed;

    public void init() {
        sheet = createImage(48, 32);
        Graphics g = sheet.getGraphics();
        g.setColor(new Color(20, 30, 40));
        g.fillRect(0, 0, 48, 32);
        g.setColor(new Color(220, 60, 40));
        g.fillRect(0, 0, 16, 16);
        g.setColor(new Color(40, 200, 90));
        g.fillRect(16, 0, 16, 16);
        g.setColor(new Color(60, 90, 230));
        g.fillRect(32, 0, 16, 16);
        g.setColor(new Color(240, 210, 60));
        g.fillRect(0, 16, 16, 16);

        /* A checker in the last quarter, so a crop landing on it is obviously
         * misaligned if it is misaligned at all. */
        for (int y = 0; y < 16; y += 2) {
            for (int x = 0; x < 32; x += 2) {
                g.setColor(((x + y) & 2) == 0 ? Color.white : Color.black);
                g.fillRect(16 + x, 16 + y, 2, 2);
            }
        }

        ImageProducer src = sheet.getSource();

        inside = createImage(new FilteredImageSource(src,
                     new CropImageFilter(16, 0, 16, 16)));

        /* Hangs eight pixels off the right and the bottom of the sheet. */
        over = createImage(new FilteredImageSource(src,
                   new CropImageFilter(40, 24, 16, 16)));

        /* A crop of a crop, reached through getSource on a filtered image. */
        chain = createImage(new FilteredImageSource(inside.getSource(),
                    new CropImageFilter(4, 4, 8, 8)));

        /* Two filters composed into one producer chain, evaluated in one go. */
        whole = createImage(new FilteredImageSource(
                    new FilteredImageSource(src,
                        new CropImageFilter(0, 0, 32, 16)),
                    new CropImageFilter(8, 2, 16, 12)));

        red = createImage(new FilteredImageSource(src, new Redden()));

        /* The base ImageFilter is the identity, and has to stay one. */
        ident = createImage(new FilteredImageSource(
                    new FilteredImageSource(src,
                        new CropImageFilter(0, 16, 16, 16)),
                    new ImageFilter()));

        int[] px = new int[24 * 24];
        for (int y = 0; y < 24; y++) {
            for (int x = 0; x < 24; x++) {
                int a = (x + y) < 8 ? 0 : 255;
                px[y * 24 + x] = (a << 24) | ((x * 10) << 16) | ((y * 10) << 8)
                                 | (((x ^ y) * 8) & 0xff);
            }
        }
        mem = createImage(new MemoryImageSource(24, 24,
                  ColorModel.getRGBdefault(), px, 0, 24));

        /* A palette and byte pixels, which is how a demo of the period does a
         * fire or a plasma. One entry has alpha zero so the ground shows. */
        byte[] r = { (byte) 0xff, 0, 0, (byte) 0x80, (byte) 0xff };
        byte[] gr = { 0, (byte) 0xff, 0, (byte) 0x80, (byte) 0xc0 };
        byte[] b = { 0, 0, (byte) 0xff, (byte) 0x80, 0 };
        byte[] al = { (byte) 0xff, (byte) 0xff, 0, (byte) 0xff, (byte) 0xff };
        byte[] idx = new byte[16 * 16];
        for (int y = 0; y < 16; y++)
            for (int x = 0; x < 16; x++)
                idx[y * 16 + x] = (byte) ((x / 4 + y / 4) % 5);
        indexed = createImage(new MemoryImageSource(16, 16,
                      new IndexColorModel(8, 5, r, gr, b, al), idx, 0, 16));

        /* A packed 5-6-5 source through a DirectColorModel, and an offset and
         * a scan wider than the image, which is the other thing these
         * constructors have to get right. */
        int[] wide = new int[20 * 16];
        for (int y = 0; y < 16; y++)
            for (int x = 0; x < 20; x++)
                wide[y * 20 + x] = ((x & 31) << 11) | ((y * 4 & 63) << 5)
                                   | ((x + y) & 31);
        packed = createImage(new MemoryImageSource(16, 12,
                     new DirectColorModel(16, 0xf800, 0x07e0, 0x001f),
                     wide, 22, 20));

        /* PixelGrabber round trip: read the sheet back out and put half of it
         * into a new image, inverted. */
        int[] got = new int[16 * 16];
        PixelGrabber pg = new PixelGrabber(sheet, 8, 4, 16, 16, got, 0, 16);
        try {
            pg.grabPixels();
        } catch (InterruptedException e) {
        }
        for (int i = 0; i < got.length; i++)
            got[i] = (got[i] & 0xff000000) | (~got[i] & 0x00ffffff);
        grabbed = createImage(new MemoryImageSource(16, 16, got, 0, 16));
    }

    public void paint(Graphics g) {
        g.setColor(Color.white);
        g.fillRect(0, 0, 300, 200);

        g.drawImage(sheet, 4, 4, this);

        g.drawImage(inside, 4, 44, this);
        g.drawImage(over, 24, 44, this);
        g.drawImage(chain, 44, 44, this);
        g.drawImage(whole, 56, 44, this);

        g.drawImage(red, 4, 68, this);
        g.drawImage(ident, 60, 68, this);

        g.drawImage(mem, 4, 108, this);

        /* Over a background colour, which is the other overload a tile with
         * transparent corners gets blitted through. */
        g.drawImage(over, 40, 108, Color.magenta, this);

        g.drawImage(indexed, 64, 108, this);
        g.drawImage(packed, 88, 108, this);
        g.drawImage(grabbed, 112, 108, this);
    }
}
