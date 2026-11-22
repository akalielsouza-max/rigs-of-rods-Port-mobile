package org.rigsofrods.mobile;

import android.app.Activity;
import android.app.ActivityManager;
import android.app.ApplicationExitInfo;
import android.graphics.Color;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.util.Arrays;
import java.util.List;

public class MainActivity extends Activity implements SurfaceHolder.Callback {
    private static native String nativeStart(Surface s, String logPath);
    private static native void nativeStop();
    private static native String nativeGetLog();

    private TextView out;
    private TextView ogreOut;
    private File statusFile;
    private Surface surface;
    private boolean ogreRunning = false;
    private boolean renderLibLoaded = false;
    private boolean okMarked = false;
    private final Handler handler = new Handler(Looper.getMainLooper());

    private final Runnable poll = new Runnable() {
        @Override
        public void run() {
            if (!ogreRunning) {
                return;
            }
            String s = nativeGetLog();
            if (s.length() > 3500) {
                s = s.substring(s.length() - 3500);
            }
            ogreOut.setText(s);
            if (!okMarked && s.contains("FRAMES_OK")) {
                okMarked = true;
                write("OGRE_OK");
            }
            handler.postDelayed(this, 500);
        }
    };

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        statusFile = new File(getFilesDir(), "load_status.txt");

        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setBackgroundColor(Color.rgb(20, 24, 30));
        root.setPadding(24, 48, 24, 24);

        LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.HORIZONTAL);
        Button b1 = new Button(this);
        b1.setText("Carregar RoR");
        Button b2 = new Button(this);
        b2.setText("Iniciar OGRE");
        Button b3 = new Button(this);
        b3.setText("Parar");
        LinearLayout.LayoutParams w = new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f);
        row.addView(b2, w);
        row.addView(b3, w);
        root.addView(row);

        SurfaceView sv = new SurfaceView(this);
        sv.getHolder().addCallback(this);
        root.addView(sv, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f));

        ScrollView scroll = new ScrollView(this);
        LinearLayout box = new LinearLayout(this);
        box.setOrientation(LinearLayout.VERTICAL);
        out = new TextView(this);
        out.setTextColor(Color.WHITE);
        out.setTextSize(12f);
        out.setTextIsSelectable(true);
        ogreOut = new TextView(this);
        ogreOut.setTextColor(Color.rgb(160, 230, 160));
        ogreOut.setTextSize(12f);
        ogreOut.setTextIsSelectable(true);
        box.addView(out);
        box.addView(ogreOut);
        scroll.addView(box);
        root.addView(scroll, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1.3f));
        setContentView(root);

        log("Rigs of Rods Mobile 0.3 (teste do OGRE)");
        log("Aparelho: " + Build.MANUFACTURER + " " + Build.MODEL
                + " | Android " + Build.VERSION.RELEASE + " (API " + Build.VERSION.SDK_INT + ")"
                + " | ABIs: " + Arrays.toString(Build.SUPPORTED_ABIS));

        String prev = read();
        if ("STARTED".equals(prev)) {
            log("ATENCAO: a tentativa anterior de carregar a libRoR.so TRAVOU o app (crash nativo).");
        } else if ("OGRE_STARTED".equals(prev)) {
            log("ATENCAO: a tentativa anterior de iniciar o OGRE TRAVOU o app (crash nativo).");
        } else if ("OGRE_OK".equals(prev)) {
            log("Tentativa anterior: o OGRE desenhou quadros normalmente.");
        } else if (prev != null) {
            log("Resultado da tentativa anterior: " + prev);
        }
        showExitReasons();

        String last = readFile(new File(getFilesDir(), "ogre_log.txt"));
        if (last != null && last.length() > 0) {
            if (last.length() > 3500) {
                last = last.substring(0, 2200) + "\n[...]\n" + last.substring(last.length() - 1200);
            }
            log("--- Log salvo da ultima tentativa do OGRE ---");
            log(last);
            log("--- fim do log salvo ---");
        }

        b1.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                tryLoadRoR();
            }
        });
        b2.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                startOgre();
            }
        });
        b3.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                stopOgre();
            }
        });
    }

    @Override
    public void surfaceCreated(SurfaceHolder holder) {
        surface = holder.getSurface();
    }

    @Override
    public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
    }

    @Override
    public void surfaceDestroyed(SurfaceHolder holder) {
        stopOgre();
        surface = null;
    }

    private void startOgre() {
        if (ogreRunning) {
            log("OGRE ja esta rodando.");
            return;
        }
        if (surface == null || !surface.isValid()) {
            log("A superficie de video ainda nao esta pronta.");
            return;
        }
        try {
            if (!renderLibLoaded) {
                System.loadLibrary("RoRRender");
                renderLibLoaded = true;
            }
        } catch (Throwable t) {
            log("ERRO ao carregar libRoRRender.so: " + t);
            return;
        }
        okMarked = false;
        write("OGRE_STARTED");
        try {
            String err = nativeStart(surface, new File(getFilesDir(), "ogre_log.txt").getPath());
            if (err != null && err.length() > 0) {
                log("ERRO: " + err);
                write("ERRO: " + err);
                return;
            }
        } catch (Throwable t) {
            log("ERRO: " + t);
            write("ERRO: " + t);
            return;
        }
        ogreRunning = true;
        log("Iniciando OGRE...");
        handler.post(poll);
    }

    private void stopOgre() {
        if (!ogreRunning) {
            return;
        }
        ogreRunning = false;
        handler.removeCallbacks(poll);
        try {
            nativeStop();
            ogreOut.setText(nativeGetLog());
        } catch (Throwable t) {
            log("ERRO ao parar: " + t);
        }
        if (!okMarked) {
            write("OGRE_PARADO_SEM_QUADROS");
        }
        log("OGRE parado.");
    }

    @Override
    protected void onDestroy() {
        stopOgre();
        super.onDestroy();
    }

    private void tryLoadRoR() {
        write("STARTED");
        log("Carregando libRoR.so...");
        new Thread(new Runnable() {
            @Override
            public void run() {
                String result;
                try {
                    System.loadLibrary("RoR");
                    result = "OK";
                } catch (Throwable t) {
                    result = "ERRO: " + t;
                }
                final String r = result;
                write(r);
                runOnUiThread(new Runnable() {
                    @Override
                    public void run() {
                        log("libRoR.so: " + r);
                    }
                });
            }
        }).start();
    }

    private void showExitReasons() {
        if (Build.VERSION.SDK_INT < 30) {
            return;
        }
        try {
            ActivityManager am = (ActivityManager) getSystemService(ACTIVITY_SERVICE);
            List<ApplicationExitInfo> list = am.getHistoricalProcessExitReasons(getPackageName(), 0, 3);
            for (ApplicationExitInfo i : list) {
                String name;
                if (i.getReason() == ApplicationExitInfo.REASON_CRASH_NATIVE) {
                    name = "CRASH_NATIVO";
                } else if (i.getReason() == ApplicationExitInfo.REASON_CRASH) {
                    name = "CRASH_JAVA";
                } else if (i.getReason() == ApplicationExitInfo.REASON_LOW_MEMORY) {
                    name = "POUCA_MEMORIA";
                } else {
                    name = "codigo " + i.getReason();
                }
                log("Saida anterior do app: " + name + " | sinal/status=" + i.getStatus()
                        + " | " + i.getDescription());
            }
        } catch (Throwable t) {
            log("(nao foi possivel ler o historico de saidas: " + t + ")");
        }
    }

    private void log(String s) {
        out.append(s + "\n");
    }

    private void write(String s) {
        try {
            FileOutputStream f = new FileOutputStream(statusFile);
            f.write(s.getBytes("UTF-8"));
            f.close();
        } catch (Throwable ignored) {
        }
    }

    private String readFile(File file) {
        try {
            if (!file.exists()) {
                return null;
            }
            FileInputStream f = new FileInputStream(file);
            byte[] b = new byte[(int) file.length()];
            int n = f.read(b);
            f.close();
            return new String(b, 0, Math.max(n, 0), "UTF-8");
        } catch (Throwable t) {
            return null;
        }
    }

    private String read() {
        try {
            if (!statusFile.exists()) {
                return null;
            }
            FileInputStream f = new FileInputStream(statusFile);
            byte[] b = new byte[(int) statusFile.length()];
            int n = f.read(b);
            f.close();
            return new String(b, 0, Math.max(n, 0), "UTF-8");
        } catch (Throwable t) {
            return null;
        }
    }
}
// FIM_DO_ARQUIVO
                
