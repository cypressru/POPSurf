/* Crossings and keys, in the Java 1.0 spelling.
 *
 * Both were implemented and neither had ever been delivered: the browser had
 * no boundary-crossing notion, and nothing in it called the key path at all.
 * This is what the two look like from inside an applet.
 *
 * Prints rather than draws so the harness can diff a transcript. An applet
 * that only drew would pass this test by doing nothing.
 */
import java.applet.Applet;
import java.awt.Color;
import java.awt.Event;
import java.awt.Graphics;

public class Cross extends Applet {

    int  enters, exits, downs, ups;
    int  lastKey  = -1;
    int  lastX    = -1, lastY = -1;
    boolean inside;

    public boolean mouseEnter(Event e, int x, int y) {
        enters++;
        inside = true;
        lastX  = x;
        lastY  = y;
        System.out.println("enter " + x + "," + y + " n=" + enters);
        return true;
    }

    public boolean mouseExit(Event e, int x, int y) {
        exits++;
        inside = false;
        lastX  = x;
        lastY  = y;
        System.out.println("exit " + x + "," + y + " n=" + exits);
        return true;
    }

    public boolean keyDown(Event e, int key) {
        downs++;
        lastKey = key;
        System.out.println("down " + name(key) + " n=" + downs);
        return true;
    }

    public boolean keyUp(Event e, int key) {
        ups++;
        System.out.println("up " + name(key) + " n=" + ups);
        return true;
    }

    /* Spelled out so a transcript says LEFT rather than 1006, and so a wrong
     * number is obvious instead of merely different. */
    static String name(int k) {
        if(k == Event.UP)    return "UP";
        if(k == Event.DOWN)  return "DOWN";
        if(k == Event.LEFT)  return "LEFT";
        if(k == Event.RIGHT) return "RIGHT";
        if(k == Event.HOME)  return "HOME";
        if(k == Event.F1)    return "F1";
        if(k >= 32 && k < 127) return "'" + (char)k + "'";
        return String.valueOf(k);
    }

    public void paint(Graphics g) {
        g.setColor(inside ? Color.yellow : Color.blue);
        g.fillRect(0, 0, 120, 40);
        g.setColor(Color.black);
        g.drawString("in=" + enters + " out=" + exits
                     + " dn=" + downs + " up=" + ups, 4, 24);
    }
}
