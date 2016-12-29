#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <arpa/inet.h>

#include <stdexcept>
#include <assert.h>

#include <openssl/rand.h>
#include <openssl/sha.h>
#include <openssl/aes.h>
#include <openssl/evp.h>

#undef BN_BYTES
#undef BN_BITS
#define rsa_st relic_rsa_st
#include "Pairing.h"

#include "dp5params.h"

extern "C" {
    int curve25519_donna(unsigned char *mypublic,
	const unsigned char *secret,
	const unsigned char *basepoint);
}

using namespace std;

namespace dp5 {

using namespace dp5::internal;

// Retrieve the current epoch number
unsigned int DP5Config::current_epoch() const
{
    if (!valid()) {
        throw runtime_error("Invalid configuration!");
    }
    return time(NULL)/epoch_len;
}



// Get public key from private key
template<>
void getpubkey<PubKey,PrivKey>(PubKey & pubkey, const PrivKey & privkey)
{
    // The generator
    static const unsigned char generator[32] = {9};

    byte pubkey_buf[pubkey.size];

    // Generate the public key
    curve25519_donna(pubkey_buf, privkey, generator);
    pubkey.assign(pubkey_buf, pubkey.size);
}

// Generate a public/private keypair
template<>
void genkeypair<PubKey,PrivKey>(PubKey & pubkey, PrivKey & privkey)
{
    // Generate a private key
    privkey.random();
    privkey[0u] &= 248;
    privkey[31u] &= 127;
    privkey[31u] |= 64;
    getpubkey(pubkey, privkey);
}

template<>
void getpubkey<BLSPubKey,Zr>(BLSPubKey & pubkey, const Zr & privkey) {
    static Pairing pairing;
    G1 g1 = pairing.g1_get_gen() ^ privkey;
    byte pubkey_buf[pubkey.size];
    g1.toBin(reinterpret_cast<char *>(pubkey_buf));
    pubkey.assign(pubkey_buf, sizeof(pubkey_buf));
}

template<>
void genkeypair<BLSPubKey,BLSPrivKey>(BLSPubKey & pubkey, BLSPrivKey & privkey)
{
    Zr privzr(true);
    byte privkey_buf[privkey.size];
    privzr.toBin(reinterpret_cast<char *>(privkey_buf));
    privkey.assign(privkey_buf, sizeof(privkey_buf));
    getpubkey(pubkey, privzr);
}


template<>
void getpubkey<BLSPubKey,BLSPrivKey>(BLSPubKey & pubkey,
    const BLSPrivKey & privkey) {
    Zr privzr;
    privzr.fromBin(reinterpret_cast<const char*>(static_cast<const byte*>(privkey)));
    getpubkey(pubkey, privzr);
}

namespace internal {
class RandSeeder {
public:
    RandSeeder() {
        unsigned char osrandbuf[32];

        // Grab 32 random bytes from the OS and use it to seed openssl's
        // PRNG.  (openssl actually seeds from /dev/urandom itself on
        // systems with one, but in case we switch to another crypto
        // library, make it explicit.)
        int urandfd = open("/dev/urandom", O_RDONLY);
        if (urandfd < 0) {
            throw runtime_error("Unable to open /dev/urandom");
        }
        int res = read(urandfd, osrandbuf, sizeof(osrandbuf));
        if (res < (int)sizeof(osrandbuf)) {
            throw runtime_error("Unable to read /dev/urandom");
        }
        close(urandfd);
        RAND_seed(osrandbuf, sizeof(osrandbuf));
    }
};

// Place num_bytes random bytes into buf.  This is not static, so that
// the PRNG can keep state if necessary
void init_seed() {
    static RandSeeder seeder;
}

void random_bytes(byte *buf, unsigned int num_bytes)
{
    // Will be constructed on first call to this function
    // FIXME: not thread-safe
    init_seed();
    RAND_bytes(buf, num_bytes);
}


template<unsigned int N>
void random_bytes(ByteArray<N> &array, unsigned int num_bytes)
{
    if (num_bytes != N) {
        throw runtime_error("Invalid call to random_bytes");
    }
    array.random();
}


template<size_t N>
void ByteArray<N>::random() {
    init_seed();
    RAND_bytes(data, N);
}

// Compute the Diffie-Hellman output for a given (buddy's) public
// key and (your own) private key
void diffie_hellman(DHOutput dh_output, const PrivKey & my_privkey,
    const PubKey & their_pubkey)
{
    curve25519_donna(dh_output, my_privkey, their_pubkey);
}

// Hash function H_1 consumes an epoch (of size EPOCH_BYTES bytes)
// and a Diffie-Hellman output (of size PUBKEY_BYTES) and produces
// a hash value of size SHAREDKEY_BYTES bytes.  H_2 consumes the
// same input and produces a hash value of size DATAKEY_BYTES bytes.
void H1H2(SharedKey H1_out, DataKey H2_out, Epoch epoch,
    const PubKey & pubkey, const DHOutput dhout)
{
    unsigned char shaout[SHA256_DIGEST_LENGTH];
    SHA256_CTX hash;
    SHA256_Init(&hash);
    SHA256_Update(&hash, "\x00", 1);
    WireEpoch wire_epoch;
    epoch_num_to_bytes(wire_epoch, epoch);
    SHA256_Update(&hash, wire_epoch, EPOCH_BYTES);
    SHA256_Update(&hash, pubkey, PUBKEY_BYTES);
    SHA256_Update(&hash, dhout, PUBKEY_BYTES);
    SHA256_Final(shaout, &hash);
    memmove(H1_out, shaout, SHAREDKEY_BYTES);
    memmove(H2_out, shaout+SHA256_DIGEST_LENGTH-DATAKEY_BYTES,
	    DATAKEY_BYTES);
}

// Hash function H_3 consumes an epoch (of size EPOCH_BYTES bytes)
// and an output of H1 (of size SHAREDKEY_BYTES bytes), and produces
// a hash value of size HASHKEY_BYTES bytes.
void H3(HashKey H3_out, Epoch epoch, const SharedKey H1_out)
{
    unsigned char shaout[SHA256_DIGEST_LENGTH];
    SHA256_CTX hash;
    SHA256_Init(&hash);
    SHA256_Update(&hash, "\x01", 1);
    WireEpoch wire_epoch;
    epoch_num_to_bytes(wire_epoch, epoch);
    SHA256_Update(&hash, wire_epoch, EPOCH_BYTES);
    SHA256_Update(&hash, H1_out, SHAREDKEY_BYTES);
    SHA256_Final(shaout, &hash);
    memmove(H3_out, shaout, HASHKEY_BYTES);
}

void H4(unsigned char H4_out[HASHKEY_BYTES],
    const unsigned char verifybytes[SIG_VERIFY_BYTES])
{
    unsigned char shaout[SHA256_DIGEST_LENGTH];
    SHA256_CTX hash;
    SHA256_Init(&hash);
    SHA256_Update(&hash, "\x02", 1);
    SHA256_Update(&hash, verifybytes, SIG_VERIFY_BYTES);
    SHA256_Final(shaout, &hash);
    memmove(H4_out, shaout, HASHKEY_BYTES);
}

void H5(unsigned char H5_out[DATAKEY_BYTES],
    Epoch epoch,
    const BLSPubKey & pubkey)
{
    unsigned char shaout[SHA256_DIGEST_LENGTH];
    SHA256_CTX hash;
    SHA256_Init(&hash);
    SHA256_Update(&hash, "\x03", 1);
    WireEpoch wire_epoch;
    epoch_num_to_bytes(wire_epoch, epoch);
    SHA256_Update(&hash, wire_epoch, EPOCH_BYTES);
    SHA256_Update(&hash, pubkey, pubkey.size);
    SHA256_Final(shaout, &hash);
    memmove(H5_out, shaout, DATAKEY_BYTES);
}




// Pseudorandom functions
// The constuctor consumes a key of size PRFKEY_BYTES bytes and
// a number of buckets (the size of the codomain of the function)
PRF::PRF(const PRFKey prfkey, unsigned int num_buckets)
    : _num_buckets(num_buckets)
{
    memmove(_prfkey, prfkey, PRFKEY_BYTES);
    if (_num_buckets < 1) {
    	_num_buckets = 1;
    }
}

// The pseudorandom function M consumes values of size
// HASHKEY_BYTES bytes, and produces values in
// {0,1,...,num_buckets-1}
unsigned int PRF::M(const HashKey x)
{
    unsigned char shaout[SHA256_DIGEST_LENGTH];
    SHA256_CTX hash;
    SHA256_Init(&hash);
    SHA256_Update(&hash, _prfkey, PRFKEY_BYTES);
    SHA256_Update(&hash, x, HASHKEY_BYTES);
    SHA256_Final(shaout, &hash);

    uint64_t outint = *(uint64_t *)shaout;
    return outint % _num_buckets;
}

static const unsigned char zeroiv[12] = {0, };

string Enc(const DataKey datakey, const string & plaintext,
    const string & additionaldata)
{
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx)
        return "";

    int ok = EVP_EncryptInit(ctx, EVP_aes_128_gcm(), (const byte *)datakey,
        zeroiv);
    if (ok != 1)
        return "";

    int len = 0;
    unsigned int ciphertext_len = 0;
    unsigned char ciphertext[plaintext.size() + 32];
    if (additionaldata.size() > 0) {
        ok = EVP_EncryptUpdate(ctx, NULL, &len,
            reinterpret_cast<const unsigned char *>(additionaldata.data()),
            additionaldata.size());
        if (ok != 1)
            return "";
    }
    ok = EVP_EncryptUpdate(ctx, ciphertext, &len,
        (const unsigned char *)plaintext.data(), plaintext.size());
    if (ok != 1)
        return "";

    ciphertext_len = len;
    ok = EVP_EncryptFinal(ctx, ciphertext+ciphertext_len, &len);
    if (ok != 1)
        return "";
    ciphertext_len += len;

    ok = EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, 16,
        ciphertext + ciphertext_len);
    if (ok != 1)
        return "";

    ciphertext_len += 16;

    assert(ciphertext_len <= sizeof(ciphertext));

    EVP_CIPHER_CTX_free(ctx);

    return string((const char *)ciphertext, ciphertext_len);
}

// Decrypt using a key of size DATAKEY_BYTES bytes a ciphertext of
// size DATAENC_BYTES bytes to yield a plaintext of size
// DATAPLAIN_BYTES.  Return 0 if the decryption was successful, -1
// otherwise.
int Dec(string & plaintext, const DataKey enckey, const string & ciphertext,
    const string & additionaldata)
{
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx)
        return 1;
    int ok = EVP_DecryptInit(ctx, EVP_aes_128_gcm(), (const byte *) enckey,
        zeroiv);
    if (!ok)
        return 2;

    unsigned char plaintext_bytes[ciphertext.size()];
    int len;
    ok = EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, 16,
        (void *) (ciphertext.data() + ciphertext.size() - 16));
    if (!ok)
        return 4;
    if (additionaldata.size() > 0) {
        ok = EVP_DecryptUpdate(ctx, NULL, &len,
            reinterpret_cast<const unsigned char *>(additionaldata.data()),
            additionaldata.size());
        if (!ok)
            return 6;
    }
    ok = EVP_DecryptUpdate(ctx, plaintext_bytes, &len,
        (const unsigned char*) ciphertext.data(), ciphertext.size()-16);
    if (!ok)
        return 3;
    int plaintext_len = len;

    ok = EVP_DecryptFinal_ex(ctx, plaintext_bytes+plaintext_len, &len);

    EVP_CIPHER_CTX_free(ctx);

    if (ok > 0) {
        plaintext_len += len;
        plaintext.assign((const char*) plaintext_bytes, plaintext_len);
        return 0;
    } else {
        return 5;
    }
}


// Convert an epoch number to an epoch byte array
void epoch_num_to_bytes(WireEpoch wire_epoch, Epoch epoch_num)
{
    unsigned int big_endian_epoch_num = htonl(epoch_num);
    memmove(wire_epoch, &big_endian_epoch_num, EPOCH_BYTES);
}

// Convert an epoch byte array to an epoch number
unsigned int epoch_bytes_to_num(const WireEpoch wire_epoch)
{
    return ntohl(*(unsigned int*)wire_epoch);
}


int hash_key_from_sig(unsigned char key[HASHKEY_BYTES],
    const unsigned char signature[EPOCH_SIG_BYTES]) {
    Pairing pairing;
    G2 sig(pairing);

    if (sig.fromBin(signature, EPOCH_SIG_BYTES) != 0) {
        return -1;
    }

    // e(g, sig)
    GT verify_token = pairing.apply(pairing.g1_get_gen(), sig);

    unsigned char verifybytes[SIG_VERIFY_BYTES];

    verify_token.toBin((char *) verifybytes);

    H4(key, verifybytes);

    return 0;
}

int hash_key_from_pk(unsigned char key[HASHKEY_BYTES],
    const BLSPubKey & pubkey,
    unsigned int epoch) {
    Pairing pairing;
    G1 pubkey_g1;
    if (pubkey_g1.fromBin(pubkey, pubkey.size) != 0) {
        return -1; // Invalid key
    }

    unsigned char E[EPOCH_BYTES];
    epoch_num_to_bytes(E, epoch);
    G2 epoch_hash(pairing, E, EPOCH_BYTES);

    GT verify_token = pairing.apply(pubkey_g1, epoch_hash);

    unsigned char verifybytes[SIG_VERIFY_BYTES];
    verify_token.toBin((char *) verifybytes);

    H4(key, verifybytes);

    return 0;
}


} // namespace internal
} // namespace dp5




#ifdef TEST_DH
#include <stdio.h>

using namespace dp5;
using namespace dp5::internal;

static void dump(const char *prefix, const unsigned char *data,
    size_t len)
{
    if (prefix) {
	printf("%s: ", prefix);
    }
    for (size_t i=0; i<len; ++i) {
	printf("%02x", data[i]);
    }
    printf("\n");
}

int main()
{
    PrivKey alice_privkey;
    PubKey alice_pubkey;
    unsigned char alice_dh[PUBKEY_BYTES];
    PrivKey bob_privkey;
    PubKey bob_pubkey;
    unsigned char bob_dh[PUBKEY_BYTES];

    genkeypair(alice_pubkey, alice_privkey);
    dump("Alice privkey ", alice_privkey, PRIVKEY_BYTES);
    dump("Alice pubkey  ", alice_pubkey, PUBKEY_BYTES);
    genkeypair(bob_pubkey, bob_privkey);
    dump("Bob   privkey ", bob_privkey, PRIVKEY_BYTES);
    dump("Bob   pubkey  ", bob_pubkey, PUBKEY_BYTES);
    diffie_hellman(alice_dh, alice_privkey, bob_pubkey);
    diffie_hellman(bob_dh, bob_privkey, alice_pubkey);
    dump("Alice DH      ", alice_dh, PUBKEY_BYTES);
    dump("Bob   DH      ", bob_dh, PUBKEY_BYTES);

    if (memcmp(alice_dh, bob_dh, PUBKEY_BYTES)) {
	printf("\nNO MATCH\n");
	return 1;
    }

    printf("\nMATCH\n");
    return 0;
}
#endif // TEST_DH

#ifdef TEST_HASHES
#include <stdio.h>

using namespace dp5;
using namespace dp5::internal;

static void dump(const char *prefix, const unsigned char *data,
    size_t len)
{
    if (prefix) {
	printf("%s: ", prefix);
    }
    for (size_t i=0; i<len; ++i) {
	printf("%02x", data[i]);
    }
    printf("\n");
}

int main()
{
    PrivKey alice_privkey, bob_privkey;
    PubKey alice_pubkey, bob_pubkey;
    unsigned char alice_dh[PUBKEY_BYTES];
    unsigned char bob_dh[PUBKEY_BYTES];
    unsigned char H1[SHAREDKEY_BYTES];
    unsigned char H2[DATAKEY_BYTES];
    unsigned char _H3[HASHKEY_BYTES];

    genkeypair(alice_pubkey, alice_privkey);
    genkeypair(bob_pubkey, bob_privkey);
    diffie_hellman(alice_dh, alice_privkey, bob_pubkey);
    diffie_hellman(bob_dh, bob_privkey, alice_pubkey);
    unsigned int epoch = 12345678;
    printf("e : %d (%08x)\n", epoch, epoch);
    dump("s ", alice_dh, PUBKEY_BYTES);
    printf("\n");
    H1H2(H1, H2,epoch, alice_pubkey, alice_dh);
    dump("H1", H1, SHAREDKEY_BYTES);
    dump("H2", H2, DATAKEY_BYTES);
    H3(_H3, epoch, H1);
    dump("H3", _H3, HASHKEY_BYTES);

    return 0;
}
#endif // TEST_HASHES

#ifdef TEST_PRF
#include <stdio.h>
#include <stdlib.h>

using namespace dp5;
using namespace dp5::internal;

int main(int argc, char **argv)
{
    unsigned int num_buckets = (argc > 1 ? atoi(argv[1]) : 10);

    const unsigned int num_prfs = 5;
    PRF *prfs[num_prfs];

    for (unsigned int i=0; i<num_prfs; ++i) {
    	unsigned char key[PRFKEY_BYTES];
    	random_bytes(key, PRFKEY_BYTES);
    	prfs[i] = new PRF(key, num_buckets);
    }

    const unsigned int num_inputs = 20;
    for (unsigned int inp=0; inp<num_inputs; ++inp) {
    	unsigned char x[HASHKEY_BYTES];
    	random_bytes(x, HASHKEY_BYTES);
    	for (unsigned int p=0; p<num_prfs; ++p) {
    	    printf("%u\t", prfs[p]->M(x));
    	}
    	printf("\n");
    }

    for (unsigned int i=0; i<num_prfs; ++i) {
    	delete prfs[i];
    }

    return 0;
}
#endif // TEST_PRF

#ifdef TEST_ENC
#include <stdio.h>

using namespace dp5;
using namespace dp5::internal;

static void dump(const char *prefix, const unsigned char *data,
    size_t len)
{
    if (prefix) {
	printf("%s: ", prefix);
    }
    for (size_t i=0; i<len; ++i) {
	printf("%02x", data[i]);
    }
    printf("\n");
}

int main()
{
    const unsigned int DATAPLAIN_BYTES = 16;

    unsigned char key1[DATAKEY_BYTES];
    unsigned char key2[DATAKEY_BYTES];
    unsigned char key3[DATAKEY_BYTES];

    random_bytes(key1, DATAKEY_BYTES);
    random_bytes(key2, DATAKEY_BYTES);
    dump("Key 1  ", key1, DATAKEY_BYTES);
    for (unsigned i = 0; i < DATAKEY_BYTES; i++) {
      key2[i] = 'a' + i;
      key3[i] = 0;
    }
    dump("Key 2  ", key2, DATAKEY_BYTES);

    string plain1, plain2, zerop;
    for (unsigned int i=0; i<DATAPLAIN_BYTES; ++i) {
	plain1.push_back('A' + i);
	plain2.push_back('0' + i);
    zerop.push_back(0);
    }
    dump("\nPlain 1", (const unsigned char *) plain1.data(), DATAPLAIN_BYTES);
    dump("Plain 2", (const unsigned char *) plain2.data(), DATAPLAIN_BYTES);
    dump("Zero P", (const unsigned char *) zerop.data(), zerop.size());

    string cipher11, cipher12, cipher21, cipher22;
    cipher11 = Enc(key1, plain1);
    cipher12 = Enc(key1, plain2);
    cipher21 = Enc(key2, plain1);
    cipher22 = Enc(key2, plain2);
    dump("\nCip 1/1", (const unsigned char *) cipher11.data(), cipher11.size());
    dump("Cip 1/2", (const unsigned char *) cipher12.data(), cipher12.size());
    dump("Cip 2/1", (const unsigned char *) cipher21.data(), cipher21.size());
    dump("Cip 2/2", (const unsigned char *) cipher22.data(), cipher22.size());
    string testvec = Enc(key3, zerop);
    dump("TV", (const unsigned char*) testvec.data(), testvec.size());

    string dec11, dec12, dec21, dec22;
    int res11 = Dec(dec11, key1, cipher11);
    int res12 = Dec(dec12, key1, cipher12);
    int res21 = Dec(dec21, key2, cipher21);
    int res22 = Dec(dec22, key2, cipher22);
    printf("\n(%d) ", res11); dump("Dec 1/1", (const unsigned char *) dec11.data(), DATAPLAIN_BYTES);
    printf("(%d) ", res12); dump("Dec 1/2", (const unsigned char *) dec12.data(), DATAPLAIN_BYTES);
    printf("(%d) ", res21); dump("Dec 2/1", (const unsigned char *) dec21.data(), DATAPLAIN_BYTES);
    printf("(%d) ", res22); dump("Dec 2/2", (const unsigned char *) dec22.data(), DATAPLAIN_BYTES);

    return 0;
}
#endif // TEST_ENC

#ifdef TEST_EPOCH
#include <stdio.h>

using namespace dp5;
using namespace dp5::internal;

static void dump(const char *prefix, const unsigned char *data,
    size_t len)
{
    if (prefix) {
	printf("%s: ", prefix);
    }
    for (size_t i=0; i<len; ++i) {
	printf("%02x", data[i]);
    }
    printf("\n");
}

int main()
{
    Epoch epoch = 12345678;
    printf("Epoch = %u\n", epoch);
    WireEpoch epoch_bytes;
    epoch_num_to_bytes(epoch_bytes, epoch);
    dump("E", epoch_bytes, EPOCH_BYTES);
    unsigned int back;
    back = epoch_bytes_to_num(epoch_bytes);
    printf("Back  = %u\n", back);
    if (back != epoch) {
	printf("NO MATCH\n");
	return 1;
    } else {
	printf("MATCH\n");
    }

    epoch_num_to_bytes(epoch_bytes, 0x12345678);
    if (memcmp(epoch_bytes, "\x12\x34\x56\x78", 4)) {
	printf("Epoch conversion failed\n");
	return 1;
    }
    if (epoch_bytes_to_num(epoch_bytes) != 0x12345678) {
	printf("Epoch reverse conversion failed\n");
	return 1;
    }

    printf("\nConversions successful\n");

    return 0;
}
#endif // TEST_EPOCH
