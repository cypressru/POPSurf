import java.applet.Applet;
import java.awt.Color;
import java.awt.Graphics;
import java.awt.Point;
import java.awt.event.InputEvent;
import java.awt.event.KeyAdapter;
import java.awt.event.KeyEvent;
import java.awt.event.MouseAdapter;
import java.awt.event.MouseEvent;
import java.awt.event.MouseMotionAdapter;

/* The shape real applets take: anonymous subclasses of the adapters, each
   arriving as its own class file. Overriding one method of five and leaving
   the rest to the adapter is the whole reason the adapters exist, so this
   overrides exactly one in the first and two in the second.

   It also reads every MouseEvent accessor an applet of the period uses, and
   the VK_ and modifier constants, so that a wrong value shows up here rather
   than in something from the wild. */
public class Adapter11 extends Applet {
    int[] mx = new int[16];
    int[] my = new int[16];
    int n = 0;
    int lastx = -1, lasty = -1;
    int keys = 0;

    public void init() {
        addMouseListener(new MouseAdapter() {
            public void mousePressed(MouseEvent e) {
                Point p = e.getPoint();

                if (n < mx.length) { mx[n] = p.x; my[n] = p.y; n++; }
                System.out.println("adapter pressed " + p.x + "," + p.y
                                   + " b1=" + ((e.getModifiers()
                                                & InputEvent.BUTTON1_MASK) != 0)
                                   + " shift=" + e.isShiftDown());
                repaint();
            }
        });

        addMouseMotionListener(new MouseMotionAdapter() {
            public void mouseMoved(MouseEvent e) {
                lastx = e.getX();
                lasty = e.getY();
                System.out.println("adapter moved " + lastx + "," + lasty);
                repaint();
            }
        });

        addKeyListener(new KeyAdapter() {
            public void keyPressed(KeyEvent e) {
                keys++;
                System.out.println("adapter keyPressed code=" + e.getKeyCode()
                                   + " char=" + (int) e.getKeyChar()
                                   + " left=" + (e.getKeyCode()
                                                 == KeyEvent.VK_LEFT));
                repaint();
            }

            public void keyTyped(KeyEvent e) {
                System.out.println("adapter keyTyped code=" + e.getKeyCode()
                                   + " char=" + (int) e.getKeyChar());
            }
        });
    }

    public void paint(Graphics g) {
        g.setColor(new Color(30, 22, 18));
        g.fillRect(0, 0, 300, 200);

        g.setColor(Color.white);
        g.drawString("marks: " + n + "  keys: " + keys, 12, 20);

        for (int i = 0; i < n; i++) {
            g.setColor(new Color(230, 140, 60));
            g.fillRect(mx[i] - 4, my[i] - 4, 9, 9);
        }

        if (lastx >= 0) {
            g.setColor(new Color(120, 200, 230));
            g.drawRect(lastx - 6, lasty - 6, 12, 12);
        }
    }
}
