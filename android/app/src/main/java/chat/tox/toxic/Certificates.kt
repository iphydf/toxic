// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright © 2026 The TokTok team.

package chat.tox.toxic

import android.util.Base64
import android.util.Log
import java.io.File
import java.security.KeyStore
import java.security.cert.Certificate
import java.security.cert.X509Certificate

/**
 * Exports the device's trusted certificates for toxic's OpenSSL. Android names the files in its
 * certificate directory with a hash OpenSSL no longer uses, so we put them all in one file.
 */
object Certificates {
    private const val TAG = "toxic"

    private fun subject(certificate: Certificate) =
        (certificate as? X509Certificate)?.subjectX500Principal?.name ?: certificate.type

    /**
     * Writes every certificate the device trusts that OpenSSL can read, including the user's, to
     * dir/cacerts.pem.
     */
    fun export(dir: File) {
        val file = File(dir, "cacerts.pem")
        val temp = File(dir, "cacerts.pem.new")

        try {
            val store = KeyStore.getInstance("AndroidCAStore").apply { load(null) }
            var written = 0

            temp.writer(Charsets.US_ASCII).use { out ->
                for (alias in store.aliases()) {
                    val certificate = store.getCertificate(alias) ?: continue
                    val der = certificate.encoded

                    // OpenSSL ignores the whole file if it can't read one of them.
                    if (!Terminal.readsCertificate(der)) {
                        Log.w(TAG, "OpenSSL can't read certificate $alias: ${subject(certificate)}")
                        continue
                    }

                    out.write("-----BEGIN CERTIFICATE-----\n")
                    out.write(Base64.encodeToString(der, Base64.DEFAULT))
                    out.write("-----END CERTIFICATE-----\n")
                    ++written
                }
            }

            Log.i(TAG, "trusting $written certificates")

            if (!temp.renameTo(file)) {
                Log.w(TAG, "can't replace $file")
            }
        } catch (e: Exception) {
            // toxic still starts, but can't fetch its node list over HTTPS.
            Log.w(TAG, "can't export the trusted certificates", e)
        }
    }
}
