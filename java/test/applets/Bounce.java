import java.applet.Applet;
import java.awt.Color;
import java.awt.Graphics;

/* Two classes plus a nested helper: Bounce, Sprite, and Trail. Nothing here
   works unless the browser walks the constant pool and fetches them. */
public class Bounce extends Applet implements Runnable {
    Sprite[] s;
    Trail tr;
    Thread t;

    public void init() {
        s = new Sprite[5];
        s[0] = new Sprite(10, 10, 3, 2, 22, Color.red);
        s[1] = new Sprite(60, 40, -2, 3, 18, Color.cyan);
        s[2] = new Sprite(120, 90, 4, -2, 26, Color.yellow);
        s[3] = new Sprite(200, 30, -3, -3, 14, Color.green);
        s[4] = new Sprite(250, 130, 2, -4, 20, Color.magenta);
        tr = new Trail();
    }

    public void start() { t = new Thread(this); t.start(); }

    public void run() {
        while (true) {
            for (int i = 0; i < s.length; i++) s[i].step(300, 200);
            tr.push(s[0].x, s[0].y);
            repaint();
            try { Thread.sleep(40); } catch (Exception e) { }
        }
    }

    public void paint(Graphics g) {
        g.setColor(Color.black);
        g.fillRect(0, 0, 300, 200);
        tr.draw(g);
        for (int i = 0; i < s.length; i++) s[i].draw(g);
    }
}
