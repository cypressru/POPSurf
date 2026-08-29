import java.applet.Applet;
import java.awt.Color;
import java.awt.Event;
import java.awt.Graphics;
import java.awt.event.MouseAdapter;
import java.awt.event.MouseEvent;
import java.awt.event.MouseListener;

/* Several listeners on one component, and what happens when they come off.

   Four presses, and the trace of who saw each one is the test. Three things
   are being pinned down, all of them checked against a real JDK first:
   listeners fire in the order they were registered; removing one from inside a
   callback leaves the rest of that same delivery alone; and removing every
   last listener does NOT bring the 1.0 overrides back. The last is the
   surprising one and the reason it is here - AWT latches a component into the
   new model on the first registration and never unlatches it.

   Expected:  press 1  first, second
              press 2  first, second (which then removes itself)
              press 3  first (which then removes itself)
              press 4  nobody, and in particular not mouseDown */
public class Listen11 extends Applet {
    int a = 0, b = 0;
    MouseListener first, second;

    public void init() {
        first = new MouseAdapter() {
            public void mousePressed(MouseEvent e) {
                a++;
                System.out.println("first  call " + a
                                   + " at " + e.getX() + "," + e.getY());
                if (a == 3) {
                    removeMouseListener(first);
                    System.out.println("  first removes itself");
                }
                repaint();
            }
        };

        second = new MouseAdapter() {
            public void mousePressed(MouseEvent e) {
                b++;
                System.out.println("second call " + b);
                if (b == 2) {
                    removeMouseListener(second);
                    System.out.println("  second removes itself");
                }
            }
        };

        addMouseListener(first);
        addMouseListener(second);
    }

    /* Must never print, at any point, including after both listeners are
       gone. */
    public boolean mouseDown(Event e, int x, int y) {
        System.out.println("BUG: 1.0 mouseDown ran");
        return true;
    }

    public void paint(Graphics g) {
        g.setColor(new Color(20, 20, 24));
        g.fillRect(0, 0, 300, 200);
        g.setColor(Color.white);
        g.drawString("first: " + a + "  second: " + b, 12, 24);
    }
}
