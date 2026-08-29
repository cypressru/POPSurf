/* A period-shaped applet: no lambdas, no generics, no library beyond
   Graphics and Color. Compiled with -release 8 the bytecode is the same
   shape javac 1.1 produced for the same source. */
import java.applet.Applet;
import java.awt.Color;
import java.awt.Graphics;

public class Chart extends Applet {
    int[] data = { 42, 78, 30, 91, 56, 68, 23 };

    public void paint(Graphics g) {
        int w = 300, h = 200;
        g.setColor(new Color(255, 255, 255));
        g.fillRect(0, 0, w, h);

        g.setColor(new Color(20, 20, 30));
        g.drawString("bytecode drew this", 14, 22);

        for (int i = 0; i < data.length; i++) {
            int bh = data[i] * 130 / 100;
            int x = 20 + i * 38;
            g.setColor(new Color(60 + i * 24, 130, 210 - i * 18));
            g.fillRect(x, 170 - bh, 26, bh);
            g.setColor(new Color(30, 30, 40));
            g.drawRect(x, 170 - bh, 25, bh - 1);
        }
        g.setColor(new Color(120, 120, 130));
        g.drawLine(14, 172, 286, 172);
    }
}
