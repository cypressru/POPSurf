/* The pathological back buffer: a new one every frame, at a size that keeps
 * changing, with the old one dropped on the floor.
 *
 * No applet from the wild does this on purpose - it is the "if the component
 * was resized, reallocate" branch taken every time - but it is the case where
 * a table of buffers either recycles or runs out, and running out paints red.
 */
import java.applet.Applet;
import java.awt.Color;
import java.awt.Graphics;
import java.awt.Image;

public class OffChurn extends Applet implements Runnable {
    Image    off;
    Graphics og;
    int      frame;
    Thread   anim;

    public void start() {
        anim = new Thread(this);
        anim.start();
    }

    public void run() {
        while (true) {
            frame++;
            repaint();
            try { Thread.sleep(20); } catch (InterruptedException e) { return; }
        }
    }

    public void paint(Graphics g) {
        int w = 100 + (frame % 5) * 20;

        off = createImage(w, 80);
        if (off == null) {
            g.setColor(Color.red);
            g.fillRect(0, 0, 300, 200);
            return;
        }
        og = off.getGraphics();

        og.setColor(Color.black);
        og.fillRect(0, 0, w, 80);
        og.setColor(Color.cyan);
        og.fillOval(frame % 40, 10, 40, 40);

        g.setColor(Color.white);
        g.fillRect(0, 0, 300, 200);
        g.drawImage(off, 10, 10, this);
    }
}
