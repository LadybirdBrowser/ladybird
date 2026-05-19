/**
 * Copyright (c) 2022, Andrew Kaster <akaster@serenityos.org>
 * <p>
 * SPDX-License-Identifier: BSD-2-Clause
 */

package org.serenityos.ladybird;

import android.content.Context;
import android.content.res.AssetManager;
import android.util.Log;

import java.io.BufferedInputStream;
import java.io.BufferedOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.io.UncheckedIOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.Paths;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;
import java.util.stream.Stream;

public class TransferAssets {
    private static final String TAG = "Ladybird";
    private static final String ARCHIVE_NAME = "ladybird-assets.zip";
    private static final String EXTRACTION_MARKER = ".ladybird-assets-extracted";

    /**
     * @return new ladybird resource root
     */
    static public String transferAssets(Context context) throws IOException {
        Context applicationContext = context.getApplicationContext();
        File assetDir = applicationContext.getFilesDir();
        if (isExtractionComplete(assetDir)) {
            return assetDir.getAbsolutePath();
        }

        cleanupIncompleteExtraction(assetDir);

        AssetManager assetManager = applicationContext.getAssets();
        extractArchive(assetManager, assetDir);
        mergeSystemCertificates(assetDir);
        createExtractionMarker(assetDir);
        Log.d(TAG, "Extracted assets directly from APK into app-specific storage");
        return assetDir.getAbsolutePath();
    }

    private static boolean isExtractionComplete(File assetDir) {
        return new File(assetDir, EXTRACTION_MARKER).exists()
            && new File(assetDir, "res/icons/48x48/app-browser.png").exists()
            && new File(assetDir, "cacert.pem").exists();
    }

    private static void extractArchive(AssetManager assetManager, File assetDir) throws IOException {
        try (ZipInputStream input = new ZipInputStream(new BufferedInputStream(assetManager.open(ARCHIVE_NAME)))) {
            ZipEntry entry;
            while ((entry = input.getNextEntry()) != null) {
                File outputFile = validateEntryPath(assetDir, entry.getName());
                if (entry.isDirectory()) {
                    if (!outputFile.exists() && !outputFile.mkdirs()) {
                        throw new IOException("Unable to create directory " + outputFile);
                    }
                    input.closeEntry();
                    continue;
                }

                File parent = outputFile.getParentFile();
                if (parent != null && !parent.exists() && !parent.mkdirs()) {
                    throw new IOException("Unable to create directory " + parent);
                }

                try (OutputStream output = new BufferedOutputStream(new FileOutputStream(outputFile))) {
                    copyFile(input, output);
                }
                input.closeEntry();
            }
        }
    }

    private static File validateEntryPath(File assetDir, String entryName) throws IOException {
        File canonicalAssetDir = assetDir.getCanonicalFile();
        File outputFile = new File(assetDir, entryName).getCanonicalFile();
        String assetDirPath = canonicalAssetDir.getPath() + File.separator;
        if (!outputFile.getPath().startsWith(assetDirPath)) {
            throw new IOException("Archive entry escapes target directory: " + entryName);
        }
        return outputFile;
    }

    private static void mergeSystemCertificates(File assetDir) throws IOException {
        File outputFile = new File(assetDir, "cacert.pem");
        Path certificateDirectory = Paths.get("/system/etc/security/cacerts");
        try (OutputStream output = new BufferedOutputStream(new FileOutputStream(outputFile));
             Stream<Path> certPaths = Files.walk(certificateDirectory)) {
            certPaths
                .filter(Files::isRegularFile)
                .forEach(certPath -> {
                    try (InputStream input = new BufferedInputStream(Files.newInputStream(certPath))) {
                        copyFile(input, output);
                    } catch (IOException exception) {
                        throw new UncheckedIOException(exception);
                    }
                });
        } catch (IOException exception) {
            throw new IOException("Unable to read Android system certificates from " + certificateDirectory, exception);
        } catch (UncheckedIOException exception) {
            throw exception.getCause();
        }
    }

    private static void cleanupIncompleteExtraction(File assetDir) throws IOException {
        deleteRecursively(new File(assetDir, "res"));
        deleteIfExists(new File(assetDir, "cacert.pem"));
        deleteIfExists(new File(assetDir, ARCHIVE_NAME));
        deleteIfExists(new File(assetDir, EXTRACTION_MARKER));
    }

    private static void createExtractionMarker(File assetDir) throws IOException {
        File marker = new File(assetDir, EXTRACTION_MARKER);
        if (!marker.createNewFile() && !marker.exists()) {
            throw new IOException("Unable to create extraction marker " + marker);
        }
    }

    private static void deleteRecursively(File file) throws IOException {
        if (!file.exists()) {
            return;
        }
        if (file.isDirectory()) {
            File[] children = file.listFiles();
            if (children != null) {
                for (File child : children) {
                    deleteRecursively(child);
                }
            }
        }
        deleteIfExists(file);
    }

    private static void deleteIfExists(File file) throws IOException {
        if (file.exists() && !file.delete()) {
            throw new IOException("Unable to delete " + file);
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
