/* The drawing half of the Polygon work, which the printing test cannot reach.
 *
 * This one is not diffed against a real JDK - two rasterisers do not agree on
 * pixels and never will. It exists so that every new path in ps_jpoly.c is
 * executed under the sanitiser: the single-argument drawPolygon and
 * fillPolygon, drawPolyline, draw3DRect and fill3DRect, drawChars and
 * drawBytes, and an addPoint loop long enough to reallocate the coordinate
 * arrays several times per frame with a collection in the middle of it.
 *
 * The polygon rebuilt in paint() is the part worth keeping: xpoints and
 * ypoints are ordinary traced arrays and growing them replaces both, so a
 * frame that allocates hard is where a mistake in the ordering would show up
 * as a use-after-free rather than as a wrong picture.
 */
import java.applet.Applet;
import java.awt.Color;
import java.awt.Graphics;
import java.awt.Polygon;

public class PolyDraw extends Applet {
    int frame;

    public void paint(Graphics g) {
        int i;

        /* Locals rather than static finals: this runtime does not run a
           class's <clinit>, so a `static final char[] = {...}` is still null
           when paint() reads it. Nothing to do with Polygon, but it would
           make this test fail for the wrong reason. */
        char[] chars = { 'P', 'o', 'l', 'y', 'g', 'o', 'n' };
        byte[] bytes = { (byte) '3', (byte) 'D', (byte) ' ', (byte) 'r',
                         (byte) 'e', (byte) 'c', (byte) 't' };

        frame++;

        g.setColor(Color.white);
        g.fillRect(0, 0, 300, 200);

        /* A filled star: self-intersecting, so the even-odd rule leaves the
           middle hollow and the fill agrees with contains(). */
        Polygon star = new Polygon();
        star.addPoint(60, 20);
        star.addPoint(36, 92);
        star.addPoint(98, 48);
        star.addPoint(22, 48);
        star.addPoint(84, 92);
        g.setColor(Color.blue);
        g.fillPolygon(star);
        g.setColor(Color.black);
        g.drawPolygon(star);

        /* Built from arrays and then moved, which is how a physics applet
           draws an arrow that points somewhere different every frame. */
        Polygon arrow = new Polygon(new int[] { 0, 30, 22, 22, 0 },
                                    new int[] { 8, 8, 0, 16, 8 }, 5);
        arrow.translate(120 + frame * 3, 30);
        g.setColor(Color.red);
        g.fillPolygon(arrow);

        /* A trace, open at both ends. */
        int[] tx = new int[32];
        int[] ty = new int[32];
        for (i = 0; i < 32; i++) {
            tx[i] = 10 + i * 9;
            ty[i] = 120 + (i % 2 == 0 ? 0 : 14) + frame;
        }
        g.setColor(Color.darkGray);
        g.drawPolyline(tx, ty, 32);

        g.setColor(Color.lightGray);
        g.draw3DRect(10, 150, 60, 30, true);
        g.fill3DRect(80, 150, 60, 30, true);
        g.draw3DRect(150, 150, 60, 30, false);
        g.fill3DRect(220, 150, 60, 30, false);

        /* Degenerate sizes, which is where the span rules differ from a pair
           of drawLine calls. */
        g.draw3DRect(285, 150, 0, 0, true);
        g.fill3DRect(290, 150, 1, 1, true);
        g.fill3DRect(295, 150, 2, 2, false);

        g.setColor(Color.black);
        g.drawChars(chars, 0, chars.length, 200, 20);
        g.drawChars(chars, 2, 3, 200, 36);
        g.drawBytes(bytes, 0, bytes.length, 200, 52);
        g.drawBytes(bytes, 3, 4, 200, 68);

        /* Grow the arrays from four to well past a thousand, twice, so the
           reallocation path runs with the collector active underneath it. */
        for (int pass = 0; pass < 2; pass++) {
            Polygon big = new Polygon();
            for (i = 0; i < 1200; i++)
                big.addPoint(i & 255, (i * 7) & 127);
            if (big.npoints != 1200 || big.xpoints.length != 2048)
                System.out.println("grow wrong " + big.npoints + " "
                                   + big.xpoints.length);
            big.reset();
            big.addPoint(1, 2);
            if (big.getBounds().width != 0)
                System.out.println("reset wrong");
        }
    }
}
