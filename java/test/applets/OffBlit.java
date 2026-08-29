/* The drawImage overloads an applet reaches for once it has a back buffer:
 * plain, scaled, and with a background colour behind the transparent parts.
 *
 * Also the calls that surround them - Image.flush, getWidth/getHeight with an
 * observer, Component.getGraphics, and a second createImage that replaces the
 * first, which is the resize case the buffer table has to survive. */
import java.applet.Applet;
import java.awt.Color;
import java.awt.Graphics;
import java.awt.Image;

public class OffBlit extends Applet {
    Image src;

    public void init() {
        /* Allocated, drawn into, then thrown away for a bigger one: exactly
         * what "if (off == null || size changed) off = createImage(...)" does
         * on the first resize. */
        Image scratch = createImage(8, 8);
        Graphics sg0 = scratch.getGraphics();
        sg0.setColor(Color.yellow);
        sg0.fillRect(0, 0, 8, 8);
        scratch.flush();
        scratch = null;

        src = createImage(64, 48);

        Graphics sg = src.getGraphics();
        sg.setColor(Color.red);
        sg.fillRect(0, 0, 32, 24);
        sg.setColor(Color.green);
        sg.fillRect(32, 0, 32, 24);
        sg.setColor(Color.blue);
        sg.fillRect(0, 24, 32, 24);
        sg.setColor(Color.yellow);
        sg.fillRect(32, 24, 32, 24);
        sg.setColor(Color.black);
        sg.drawRect(0, 0, 63, 47);
        sg.drawLine(0, 0, 63, 47);

        Graphics cg = getGraphics();     /* must not be null, must not throw */
        if (cg != null)
            cg.dispose();
    }

    public void paint(Graphics g) {
        if (src == null)                 /* a real AWT paints before init() */
            return;

        g.setColor(Color.white);
        g.fillRect(0, 0, 300, 200);

        /* Plain. */
        g.drawImage(src, 4, 4, this);

        /* Scaled by an exact integer, and by a fraction. */
        g.drawImage(src, 80, 4, src.getWidth(this) * 2,
                    src.getHeight(this) * 2, this);
        g.drawImage(src, 4, 120, 45, 30, this);

        /* With a background. Our offscreen images are opaque, so this has to
         * come out the same as the plain form - the bg is only ever visible
         * through a transparent source. */
        g.drawImage(src, 80, 120, Color.magenta, this);
    }
}
