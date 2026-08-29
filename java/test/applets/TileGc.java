/* A producer graph that has to survive the collector.
 *
 * The hazard this exists for: an applet builds its tiles in init() and does not
 * touch them again until paint(), which is several collections later, and the
 * recipe for each tile is a graph of Java objects - a FilteredImageSource
 * holding an upstream producer holding the sheet. If any link of that were
 * held in C rather than in an instance slot the collector would not see it,
 * and the tile would come back blank or worse on the frame after a collection.
 *
 * So: hundreds of tiles are made and dropped to drive collections, one tile is
 * kept in a field across all of them, and paint() draws the kept one. The
 * picture is the check - a wrong answer here is a blank square, not a crash -
 * and running it under java/test/asanrun.sh is what turns a freed producer
 * into a report rather than a plausible frame.
 */
import java.applet.Applet;
import java.awt.Color;
import java.awt.Graphics;
import java.awt.Image;
import java.awt.image.CropImageFilter;
import java.awt.image.FilteredImageSource;
import java.awt.image.MemoryImageSource;

public class TileGc extends Applet {
    Image sheet, kept, deep;

    public void init() {
        int[] px = new int[64 * 64];
        for (int y = 0; y < 64; y++)
            for (int x = 0; x < 64; x++)
                px[y * 64 + x] = 0xff000000 | ((x * 4) << 16) | ((y * 4) << 8)
                                 | ((x ^ y) & 0xff);
        sheet = createImage(new MemoryImageSource(64, 64, px, 0, 64));

        kept = createImage(new FilteredImageSource(sheet.getSource(),
                   new CropImageFilter(8, 8, 32, 32)));

        /* Four filters deep, so the whole chain has to stay reachable through
         * the one reference the applet keeps. */
        deep = createImage(new FilteredImageSource(
                   new FilteredImageSource(
                       new FilteredImageSource(
                           new FilteredImageSource(sheet.getSource(),
                               new CropImageFilter(0, 0, 48, 48)),
                           new CropImageFilter(4, 4, 40, 40)),
                       new CropImageFilter(4, 4, 32, 32)),
                   new CropImageFilter(4, 4, 24, 24)));

        /* Enough throwaway work to take the heap past its threshold several
         * times over. Each of these is a live producer graph until the
         * iteration after it. */
        for (int i = 0; i < 400; i++) {
            Image t = createImage(new FilteredImageSource(sheet.getSource(),
                          new CropImageFilter(i % 32, i % 32, 24, 24)));
            if (t.getWidth(this) != 24)
                System.out.println("width " + t.getWidth(this));
        }
    }

    public void paint(Graphics g) {
        g.setColor(Color.white);
        g.fillRect(0, 0, 300, 200);
        g.drawImage(sheet, 4, 4, this);
        g.drawImage(kept, 72, 4, this);
        g.drawImage(deep, 112, 4, this);
        g.drawImage(kept, 4, 72, 64, 64, this);
    }
}
