/**
 * Copyright (c) 2022, Andrew Kaster <akaster@serenityos.org>
 * <p>
 * SPDX-License-Identifier: BSD-2-Clause
 */

package org.serenityos.ladybird;

import android.content.Context;
import android.content.res.AssetManager;
import android.util.Log;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;

import java.lang.String;

public class TransferAssets {
    /**
     * @return new ladybird resource root
     */
    static public String transferAssets(Context context) {
        Log.d("Ladybird", "Hello from java");
        Context applicationContext = context.getApplicationContext();
        File assetDir = applicationContext.getFilesDir();
        AssetManager assetManager = applicationContext.getAssets();
        if (!copyAsset(assetManager, "ladybird-assets.tar", assetDir.getAbsolutePath() + "/ladybird-assets.tar")) {
            Log.e("Ladybird", "Unable to copy assets");
            return "Invalid Assets, this won't work";
        }
        Log.d("Ladybird", "Copied ladybird-assets.tar to app-specific storage path");
        return assetDir.getAbsolutePath();
    }

    // ty to https://stackoverflow.com/a/22903693 for the sauce
    private static boolean copyAsset(AssetManager assetManager,
                                     String fromAssetPath, String toPath) {
        try {
            InputStream in = assetManager.open(fromAssetPath);
            new File(toPath).createNewFile();
            OutputStream out = new FileOutputStream(toPath);
            copyFile(in, out);
            in.close();
            out.flush();
            out.close();
            return true;
        } catch (Exception e) {
            e.printStackTrace();
            return false;
        }
    }

    private static void copyFile(InputStream in, OutputStream out) throws IOException {
        byte[] buffer = new byte[4096];
        int read;
        while ((read = in.read(buffer)) != -1) {
            out.write(buffer, 0, read);
        }
    }
}
