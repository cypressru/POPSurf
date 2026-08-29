/* Two Graphics on one offscreen image have to be independent, because the
 * real API says so and because an applet that translates one of them would
 * otherwise move everything else it draws that frame.
 *
 * The blue and green blocks are drawn through the second context after the
 * first has been translated and clipped; if the contexts shared state they
 * would move with it, or vanish into the clip. The red block is drawn through
 * the first and must land inside 100,50..140,90 - translated, and cut down
 * from 80x80 by a clip that can only ever narrow. */
import java.applet.Applet;
import java.awt.Color;
import java.awt.Graphics;
import java.awt.Image;

public class OffCtx extends Applet {
    Image off;

    public void init() {
        off = createImage(300, 200);

        Graphics a = off.getGraphics();
        Graphics b = off.getGraphics();

        a.setColor(Color.white);
        a.fillRect(0, 0, 300, 200);

        a.translate(100, 50);
        a.clipRect(0, 0, 40, 40);
        a.setColor(Color.red);
        a.fillRect(0, 0, 80, 80);

        b.setColor(Color.blue);
        b.fillRect(0, 0, 30, 30);
        b.setColor(Color.green);
        b.fillRect(200, 150, 40, 40);

        /* A third, after the other two have been used, must still start clean. */
        Graphics c = off.getGraphics();
        c.setColor(Color.black);
        c.drawRect(0, 0, 299, 199);
    }

    public void paint(Graphics g) {
        if (off == null)
            return;
        g.drawImage(off, 0, 0, this);
    }
}
