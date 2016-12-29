#ifndef __DP5_METADATA__
#define __DP5_METADATA__

#include <string>
#include <iostream>

#include "dp5params.h"

namespace dp5 {

    namespace internal {
        // Metadata for a given database

        static const unsigned int UINT_BYTES = 4;
        static const unsigned int METADATA_VERSION = 0x02;
        class Metadata : public DP5Config {
        public:
            PRFKey prfkey;
            unsigned int epoch;
            unsigned int num_buckets;
            unsigned int bucket_size;

            Metadata(std::istream & is);

            Metadata(const std::string & metadata);

            Metadata();  // Sets default values

            Metadata(const Metadata & other);

            Metadata(const DP5Config & config);

            int fromStream(std::istream & is);
            int fromString(const std::string & str);

            void toStream(std::ostream & os) const;
            std::string toString(void) const;
        };

        unsigned int read_uint(std::istream & is);
        void write_uint(std::ostream & os, unsigned int n);
        unsigned int read_epoch(std::istream & is);
        void write_epoch(std::ostream & os, unsigned int epoch);

        // Convert an uint number to an uint byte array in network order
        void uint_num_to_bytes(unsigned char uint_bytes[UINT_BYTES],
           unsigned int uint_num);

        // Convert an uint byte array in networkorder to an uint number
        unsigned int uint_bytes_to_num(
            const unsigned char uint_bytes[UINT_BYTES]);

    }
}

#endif
