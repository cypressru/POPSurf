import java.applet.Applet;
import java.awt.Color;
import java.awt.Graphics;

/* Exercises the opcodes a bar chart never touches: tableswitch, a 2D array
   via multianewarray, and 64-bit arithmetic including shifts and masks. */
public class Modes extends Applet {
    public void paint(Graphics g) {
        g.setColor(Color.black);
        g.fillRect(0, 0, 300, 200);

        int[][] cell = new int[9][14];
        for (int y = 0; y < 9; y++)
            for (int x = 0; x < 14; x++)
                cell[y][x] = (x * 7 + y * 13) % 5;

        for (int y = 0; y < 9; y++) {
            for (int x = 0; x < 14; x++) {
                int px = 12 + x * 20, py = 10 + y * 19;
                switch (cell[y][x]) {
                    case 0:
                        g.setColor(Color.red);
                        g.fillRect(px, py, 15, 13);
                        break;
                    case 1:
                        g.setColor(Color.cyan);
                        g.fillOval(px, py, 15, 13);
                        break;
                    case 2:
                        g.setColor(Color.yellow);
                        g.drawRect(px, py, 14, 12);
                        break;
                    case 3:
                        g.setColor(Color.green);
                        g.drawLine(px, py, px + 14, py + 12);
                        g.drawLine(px, py + 12, px + 14, py);
                        break;
                    default:
                        g.setColor(Color.magenta);
                        g.fillArc(px, py, 15, 13, 30, 240);
                        break;
                }
            }
        }

        long acc = 1L;
        for (int i = 1; i <= 20; i++)
            acc = acc * i / (i > 10 ? 2L : 1L);
        long masked = (acc << 3) & 0xFFFFFFL;

        int r = (int)((masked >> 16) & 0xFF);
        int gg = (int)((masked >> 8) & 0xFF);
        int b = (int)(masked & 0xFF);
        g.setColor(new Color(r, gg, b));
        g.fillRect(12, 188, 276, 8);

        g.setColor(Color.white);
        g.drawString("tableswitch  int[][]  long shifts", 12, 184);
    }
}
