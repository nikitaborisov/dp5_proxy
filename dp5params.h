#ifndef __DP5PARAMS_H__
#define __DP5PARAMS_H__

#include <string>

namespace dp5 {
    // The mamimum number of buddies per client
    static const unsigned int MAX_BUDDIES = 100;

    // Number of bytes in a private key
    static const unsigned int PRIVKEY_BYTES = 32;
    typedef char PrivKey[PRIVKEY_BYTES];

    // Number of bytes in a public key
    static const unsigned int PUBKEY_BYTES = 32;
    typedef char PubKey[PUBKEY_BYTES];

    ///
    /// Generate a public-private keypair
    ///
    void genkeypair(PubKey pubkey, PrivKey privkey);

    // Compute a public key from a private key
    void getpubkey(PubKey pubkey, const PrivKey privkey);

    // Epoch representation
    typedef unsigned int Epoch;



    // these constants should not be necessary for
    // DP5 clients to understand
    namespace internal {
        // Number of bytes in a shared key
        static const unsigned int SHAREDKEY_BYTES = 10;
        typedef char SharedKey[SHAREDKEY_BYTES];

        // Number of bytes in a hashed shared key
        static const unsigned int HASHKEY_BYTES = 10;
        typedef char HashKey[HASHKEY_BYTES];
        // FIXME: this could be configurable?

        // Number of bytes in the key that encrypts the associated data
        static const unsigned int DATAKEY_BYTES = 16;
        typedef char DataKey[DATAKEY_BYTES];

        // The number of PIR words per byte.  This is 8 for Chor et al.'s
        // super-simple PIR scheme, and 1 for Goldberg's scheme over GF(2^8)
        static const unsigned int PIR_WORDS_PER_BYTE = 1;

        // Number of bytes in a key for the pseudorandom function family
        static const unsigned int PRFKEY_BYTES = 8;
        typedef char PRFKey[PRFKEY_BYTES];

        // The length of a byte-array version of an epoch number
        static const unsigned int EPOCH_BYTES = 4;  // epochs are 32 bit
        typedef char WireEpoch[EPOCH_BYTES];

        // Place num_bytes random bytes into buf.  This is not static, so that
        // the PRNG can keep state if necessary
        void random_bytes(unsigned char *buf, unsigned int num_bytes);

        typedef char DHOutput[PUBKEY_BYTES];

        // Compute the Diffie-Hellman output for a given (buddy's) public
        // key and (your own) private key
        void diffie_hellman(DHOutput dh_output, const PrivKey my_privkey,
        	const PubKey their_pubkey);

        // Hash function H_1 consumes an epoch (of size EPOCH_BYTES bytes)
        // and a Diffie-Hellman output (of size PUBKEY_BYTES) and produces
        // a hash value of size SHAREDKEY_BYTES bytes.  H_2 consumes the
        // same input and produces a hash value of size DATAKEY_BYTES bytes.
        void H1H2(SharedKey H1_out, DataKey H2_out, Epoch epoch,
            PubKey pubkey, DHOutput dh_output);

        // Hash function H_3 consumes an epoch (of size EPOCH_BYTES bytes)
        // and an output of H1 (of size SHAREDKEY_BYTES bytes), and produces
        // a hash value of size HASHKEY_BYTES bytes.
        void H3(HashKey H3_out, Epoch epoch, SharedKey H1_out);

        // Pseudorandom functions
        class PRF {
        public:
        	// The constuctor consumes a key of size PRFKEY_BYTES bytes and
        	// a number of buckets (the size of the codomain of the function)
        	PRF(const PRFKey prfkey, unsigned int num_buckets);

        	// The pseudorandom function M consumes values of size
        	// HASHKEY_BYTES bytes, and produces values in
        	// {0,1,...,num_buckets-1}
        	unsigned int M(const HashKey hashkey);

        	// A destructor is unnecessary for our implementation
        	//~PRF();

        private:
        	// Store a copy of the key
        	PRFKey _prfkey;

        	// Store a copy of the output size
        	unsigned int _num_buckets;
        };

        // Encryption and decryption of associated data
        // Each (small) piece of associated data is encrypted with a
        // different key, so keeping key state is unnecessary.

        // Encrypt using a key of size DATAKEY_BYTES bytes a plaintext of size
        // DATAPLAIN_BYTES bytes to yield a ciphertext of size DATAENC_BYTES
        // bytes.
        std::string Enc(const DataKey datakey, const std::string & plaintext);

        // Decrypt using a key of size DATAKEY_BYTES bytes a ciphertext of
        // size DATAENC_BYTES bytes to yield a plaintext of size
        // DATAPLAIN_BYTES.  Return 0 if the decryption was successful, -1
        // otherwise.
        int Dec(std::string & plaintext, const DataKey enckey,
            const std::string & ciphertext);


        // Convert an epoch number to an epoch byte array
        void epoch_num_to_bytes(WireEpoch result, Epoch epoch_num);

        // Convert an epoch byte array to an epoch number
        Epoch epoch_bytes_to_num(const WireEpoch wire_epoch);
    } // dp5::internal

} // dp5

#endif
