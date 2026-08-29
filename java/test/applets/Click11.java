import java.applet.Applet;
import java.awt.Color;
import java.awt.Event;
import java.awt.Graphics;
import java.awt.event.MouseEvent;
import java.awt.event.MouseListener;
import java.awt.event.MouseMotionListener;

/* The Java 1.1 twin of Click.java: the same picture, driven by listeners the
   applet registers on itself instead of by overriding mouseDown.

   The 1.0 overrides are still here and must NOT fire. Real AWT retires them
   the moment anything is registered, so if either of them prints, the browser
   is delivering every click twice. */
public class Click11 extends Applet
        implements MouseListener, MouseMotionListener {

    int[] mx = new int[16];
    int[] my = new int[16];
    int n = 0, clicks = 0, releases = 0, clicked = 0;
    int lastx = -1, lasty = -1;

    public void init() {
        addMouseListener(this);
        addMouseMotionListener(this);
    }

    /* --- 1.1 --- */

    public void mousePressed(MouseEvent e) {
        clicks++;
        if (n < mx.length) { mx[n] = e.getX(); my[n] = e.getY(); n++; }
        System.out.println("pressed " + e.getX() + "," + e.getY()
                           + " clicks=" + e.getClickCount()
                           + " mods=" + e.getModifiers()
                           + " id=" + e.getID()
                           + " src=" + (e.getSource() == this));
        repaint();
    }

    public void mouseReleased(MouseEvent e) {
        releases++;
        System.out.println("released " + e.getX() + "," + e.getY()
                           + " clicks=" + e.getClickCount());
    }

    public void mouseClicked(MouseEvent e) {
        clicked++;
        System.out.println("clicked clicks=" + e.getClickCount());
    }

    public void mouseEntered(MouseEvent e) { System.out.println("entered"); }
    public void mouseExited(MouseEvent e)  { System.out.println("exited"); }

    public void mouseMoved(MouseEvent e) {
        lastx = e.getX();
        lasty = e.getY();
        System.out.println("moved " + lastx + "," + lasty);
        repaint();
    }

    public void mouseDragged(MouseEvent e) {
        System.out.println("dragged " + e.getX() + "," + e.getY());
    }

    /* --- 1.0, which must stay silent --- */

    public boolean mouseDown(Event e, int x, int y) {
        System.out.println("BUG: 1.0 mouseDown also ran");
        return true;
    }

    public boolean mouseMove(Event e, int x, int y) {
        System.out.println("BUG: 1.0 mouseMove also ran");
        return true;
    }

    public void paint(Graphics g) {
        g.setColor(new Color(18, 22, 30));
        g.fillRect(0, 0, 300, 200);

        g.setColor(Color.white);
        g.drawString("clicks: " + clicks, 12, 20);

        for (int i = 0; i < n; i++) {
            int v = 255 - i * 12;
            if (v < 60) v = 60;
            g.setColor(new Color(v, 194, 75));
            g.fillOval(mx[i] - 6, my[i] - 6, 13, 13);
            g.setColor(Color.white);
            g.drawOval(mx[i] - 6, my[i] - 6, 12, 12);
        }

        if (lastx >= 0) {
            g.setColor(new Color(90, 170, 210));
            g.drawLine(lastx - 8, lasty, lastx + 8, lasty);
            g.drawLine(lastx, lasty - 8, lastx, lasty + 8);
        }
    }
}
