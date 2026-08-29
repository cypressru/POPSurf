import java.awt.Color;
import java.awt.Graphics;

/* A third class, referenced only by Bounce - so it is reached on the second
   round of the constant-pool walk, not the first. */
public class Trail {
    int[] px = new int[24];
    int[] py = new int[24];
    int n = 0;

    void push(int x, int y) {
        for (int i = px.length - 1; i > 0; i--) { px[i] = px[i-1]; py[i] = py[i-1]; }
        px[0] = x; py[0] = y;
        if (n < px.length) n++;
    }

    void draw(Graphics g) {
        for (int i = 1; i < n; i++) {
            int v = 200 - i * 8;
            if (v < 20) v = 20;
            g.setColor(new Color(v, v / 3, 20));
            g.fillOval(px[i] + 8, py[i] + 8, 6, 6);
        }
    }
}
