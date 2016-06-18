#include "stdafx.h"
#include "crypto.h"
#include "Logging.h"
#include "ICrypoLoader.h"

#include <assert.h>

#include <openssl/pem.h>
#include <openssl/conf.h>
#include <openssl/x509v3.h>
bool SL::Remote_Access_Library::Crypto::INTERNAL::Started = false;


namespace SL {
	namespace Remote_Access_Library {
		namespace Crypto {
			//struct to ensure proper cleanup
			class CryptoKeys {
			public:
				CryptoKeys() {
					x509Certificate = X509_new();
					PrivateKey = EVP_PKEY_new();
				}
				~CryptoKeys() {
					if (x509Certificate) X509_free(x509Certificate);
					if (PrivateKey) EVP_PKEY_free(PrivateKey);
					if (certfile) fclose(certfile);
					if (priv_keyfile) fclose(priv_keyfile);
				}
				X509* x509Certificate = nullptr;
				EVP_PKEY* PrivateKey = nullptr;
				FILE* certfile = nullptr;
				FILE* priv_keyfile = nullptr;
			};
			class DhParamRAII {
			public:
				DhParamRAII() {
					dh = DH_new();
				}
				~DhParamRAII() {
					if (outfile) BIO_free_all(outfile);
					if (dh) DH_free(dh);
				}

				DH *dh = nullptr;
				BIO *outfile = nullptr;
			};
		
	
		}

	}
}


int add_ext(X509 *cert, int nid, char *value)
{
	X509_EXTENSION *ex;
	X509V3_CTX ctx;
	/* This sets the 'context' of the extensions. */
	/* No configuration database */
	X509V3_set_ctx_nodb(&ctx);
	/* Issuer and subject certs: both the target since it is self signed,
	* no request and no CRL
	*/
	X509V3_set_ctx(&ctx, cert, cert, NULL, NULL, 0);
	ex = X509V3_EXT_conf_nid(NULL, &ctx, nid, value);
	if (!ex)
		return 0;

	X509_add_ext(cert, ex, -1);
	X509_EXTENSION_free(ex);
	return 1;
}
/*
Taken from http://www.cryptopp.com/wiki/Security_level#Comparable_Algorithm_Strengths
bits below stength
 if bits == 2048, the strength is comparable to 112 encryption
 if bits == 3072, the strength is comparable to 128 encryption
 if bits == 7680, the strength is comparable to 192 encryption
 if bits == 15360, the strength is comparable to 256 encryption

*/

SL::Remote_Access_Library::Crypto::CertSaveLocation SL::Remote_Access_Library::Crypto::CreateCertificate(const CertInfo& info) {
	assert(INTERNAL::Started);//you must ensure proper startup of the encryption library!
	CryptoKeys cry;
	CertSaveLocation svloc;
	SL_RAT_LOG(Utilities::Logging_Levels::INFO_log_level, "Starting to generate a certifiate and private key!");
	if (!cry.x509Certificate || !cry.PrivateKey) {
		SL_RAT_LOG(Utilities::Logging_Levels::ERROR_log_level, "Failed to allocate space for the certificate and private key!");
		return svloc;
	}

	auto rsa = RSA_generate_key(info.bits, RSA_F4, NULL, NULL);

	if (!EVP_PKEY_assign_RSA(cry.PrivateKey, rsa)) {
		SL_RAT_LOG(Utilities::Logging_Levels::ERROR_log_level, "Failed EVP_PKEY_assign_RSA");
		return svloc;
	}
    
	X509_set_version(cry.x509Certificate, 2);
	ASN1_INTEGER_set(X509_get_serialNumber(cry.x509Certificate), info.Serial);
	X509_gmtime_adj(X509_get_notBefore(cry.x509Certificate), 0);
	X509_gmtime_adj(X509_get_notAfter(cry.x509Certificate), (long)60 * 60 * 24 * info.DaysValid);
	X509_set_pubkey(cry.x509Certificate, cry.PrivateKey);

	auto name = X509_get_subject_name(cry.x509Certificate);

	/* This function creates and adds the entry, working out the
	* correct string type and performing checks on its length.
	* Normally we'd check the return value for errors...
	*/
	X509_NAME_add_entry_by_txt(name, "C", MBSTRING_ASC, (const unsigned char*)info.country.c_str(), -1, -1, 0);
	X509_NAME_add_entry_by_txt(name, "O", MBSTRING_ASC, (const unsigned char*)info.companyname.c_str(), -1, -1, 0);
	X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, (const unsigned char*)info.commonname.c_str(), -1, -1, 0);

	/* Its self signed so set the issuer name to be the same as the
	* subject.
	*/
	X509_set_issuer_name(cry.x509Certificate, name);

	/* Add various extensions: standard extensions */
    
	add_ext(cry.x509Certificate, NID_basic_constraints, (char*)"critical,CA:TRUE");
	add_ext(cry.x509Certificate, NID_key_usage, (char*)"critical,keyCertSign,cRLSign");

	add_ext(cry.x509Certificate, NID_subject_key_identifier, (char*)"hash");

	/* Some Netscape specific extensions */
	add_ext(cry.x509Certificate, NID_netscape_cert_type, (char*)"sslCA");

	add_ext(cry.x509Certificate, NID_netscape_comment, (char*)"example comment extension");

	if (!X509_sign(cry.x509Certificate, cry.PrivateKey, EVP_sha256())) {
		SL_RAT_LOG(Utilities::Logging_Levels::ERROR_log_level, "Failed to sign the certifiate!");
		return svloc;
	}


	std::string saveloc = info.savelocation;
	if (saveloc.back() != '/' && saveloc.back() != '\\') saveloc += '/';

	assert(!saveloc.empty());
	svloc.Private_Key = saveloc + info.filename + "_private.pem";
    cry.priv_keyfile = fopen(svloc.Private_Key.c_str(), "wb");
    if(cry.priv_keyfile!=NULL){
		SL_RAT_LOG(Utilities::Logging_Levels::ERROR_log_level, "Failed to open the Private Key File '" << svloc.Private_Key << "' for writing!");
		svloc.Private_Key = "";
		return svloc;
	}

	PEM_write_PrivateKey(
		cry.priv_keyfile,                  /* write the key to the file we've opened */
		cry.PrivateKey,               /* our key from earlier */
		EVP_des_ede3_cbc(), /* default cipher for encrypting the key on disk */
		(unsigned char*)info.password.c_str(),       /* passphrase required for decrypting the key on disk */
		static_cast<int>(info.password.size()),                 /* length of the passphrase string */
		NULL,               /* callback for requesting a password */
		NULL                /* data to pass to the callback */
	);
	svloc.Certificate = saveloc + info.filename + "_cert.crt";

	cry.certfile = fopen(svloc.Certificate.c_str(), "wb");
    if(cry.certfile!=NULL){
		SL_RAT_LOG(Utilities::Logging_Levels::ERROR_log_level, "Failed to open the Certificate File '" << svloc.Certificate << "' for writing!");
		svloc.Certificate = "";
		return svloc;
	}
    return svloc;
}

std::string SL::Remote_Access_Library::Crypto::Createdhparams(std::string savelocation, std::string filename, int bits)
{
	assert(INTERNAL::Started);//you must ensure proper startup of the encryption library!
	std::string saveloc = savelocation;
	if (saveloc.back() != '/' && saveloc.back() != '\\') saveloc += '/';
	assert(!saveloc.empty());

	DhParamRAII cry;
	auto keyloc = saveloc + filename + "_dhparams.pem";

	if (!DH_generate_parameters_ex(cry.dh, bits, 2, NULL)) return std::string("");
	cry.outfile = BIO_new(BIO_s_file());
	if (BIO_write_filename(cry.outfile, (void*)keyloc.c_str()) <= 0) return std::string("");
	int i = 0;
	if (cry.dh->q)
		i = PEM_write_bio_DHxparams(cry.outfile, cry.dh);
	else
		i = PEM_write_bio_DHparams(cry.outfile, cry.dh);
	if (!i) return std::string("");
	return keyloc;
}

std::string SL::Remote_Access_Library::Crypto::ValidateCertificate(ICrypoLoader* certficate)
{
	
	if(!certficate)return std::string("No certificate data available!");
	auto b = certficate->get_buffer();
	if (!b) return std::string("No certificate data available!");

	std::string ret;
	auto mem = BIO_new(BIO_s_mem());
	BIO_puts(mem, b);
	auto cert = PEM_read_bio_X509(mem, NULL, NULL, NULL);
	if(cert ==NULL)  ret+= std::string("Loaded the Certifiate, but could not read the certificate information. It might be invalid!");
	if (mem) BIO_free(mem);
	if (cert) X509_free(cert);
	return ret;
}

std::string SL::Remote_Access_Library::Crypto::ValidatePrivateKey(ICrypoLoader* private_key, std::string & password)
{
	
	if (!private_key)return std::string("No Private Key data available!");
	auto b = private_key->get_buffer();
	if (!b) return std::string("No Private Key data available!");

	std::string ret;
	auto mem = BIO_new(BIO_s_mem());
	BIO_puts(mem, b);
	auto privkey = PEM_read_bio_PrivateKey(mem, NULL, NULL, (void*)password.c_str());
	if (privkey == NULL)  ret += std::string("Loaded the Private Key, but there was an error. Either the private key is invalid or the password is!");
	if (mem) BIO_free(mem);
	if (privkey) EVP_PKEY_free(privkey);
	return ret;
}

SL::Remote_Access_Library::Crypto::Initer::Initer()
{
	OpenSSL_add_all_algorithms();
	INTERNAL::Started = true;
}

SL::Remote_Access_Library::Crypto::Initer::~Initer()
{
	CRYPTO_cleanup_all_ex_data();
	EVP_cleanup();
}
