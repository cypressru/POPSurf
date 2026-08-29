import java.applet.Applet;
import java.awt.Color;
import java.awt.Event;
import java.awt.Graphics;

/* The Java 1.0 model, reading the Event object rather than the arguments.

   The coordinates come in as arguments and every applet uses those, but the
   modifiers, the click count and the id are only on the Event - `evt.clickCount
   == 2` and `evt.shiftDown()` are how a 1.0 applet spots a double click and a
   modified one, and `evt.key == Event.LEFT` is the only way it can steer
   anything, since an arrow is not a character.

   This is here because all of those used to answer zero. Nothing about the
   listener work needs it; it is the 1.0 half of the same fix. */
public class Event10 extends Applet {
    String last = "-";
    int hits = 0;

    public boolean mouseDown(Event e, int x, int y) {
        hits++;
        last = "down id=" + e.id + " x=" + e.x + " y=" + e.y
             + " clicks=" + e.clickCount + " mods=" + e.modifiers
             + " shift=" + e.shiftDown()
             + " argsxy=" + x + "," + y
             + " target=" + (e.target == this);
        System.out.println(last);
        repaint();
        return true;
    }

    public boolean keyDown(Event e, int key) {
        hits++;
        last = "key id=" + e.id + " key=" + e.key
             + " left=" + (e.key == Event.LEFT)
             + " arg=" + key;
        System.out.println(last);
        repaint();
        return true;
    }

    public void paint(Graphics g) {
        g.setColor(new Color(24, 20, 20));
        g.fillRect(0, 0, 300, 200);
        g.setColor(Color.white);
        g.drawString("events: " + hits, 12, 24);
        g.drawString(last, 12, 44);
    }
}
