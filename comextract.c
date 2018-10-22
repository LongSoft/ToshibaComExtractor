#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// Toshiba COM file header
#pragma pack(push, 1)
typedef struct TOSHIBA_COM_HEADER_ {
	uint16_t Zero;
	uint8_t  HeaderVersion;
	uint32_t Signature; // 'BIOS'
	uint16_t Unk0; 
	uint16_t Unk1;
	uint8_t  BiosVersion[16];
	uint8_t  Compressed; // 0 if not compressed, 1 if compressed
	uint32_t Unk2;
	uint32_t Unk3;
	uint32_t CompressedSize;
	uint16_t DecompressedSizeShifted; // Stored as uint32_t shifted left by 10 bits, limited to 4 Mb
	// The rest of the header is not used and can be ignored
} TOSHIBA_COM_HEADER;
#pragma pack(pop)

#define TOSHIBA_COM_HEADER_SIGNATURE 0x534F4942 // 'BIOS'
#define TOSHIBA_COM_HEADER_VERSION_0_HEADER_SIZE 0x100
#define TOSHIBA_COM_HEADER_VERSION_2_HEADER_SIZE 0x200

// Global state
static unsigned char* gCurrentInput;
static unsigned char* gCurrentOutput;
static size_t gNumDecodedBytes;
static uint16_t gBigTable[1024];
static uint16_t gSmallTable[1024];

uint8_t check(uint8_t *input, uint8_t *output)
{
	uint8_t result;

	result = (output[1] & 0x80u) != 0;
	*(uint16_t *)output *= 2;
	if ((++input[1] & 8) == 8) {
		input[1] = 0;
		*output = *gCurrentInput++;
	}

	return result;
}

void apply(uint8_t *input, uint8_t *output, uint16_t *index)
{
	*index = output[1];
	*(uint16_t *)output <<= 8 - input[1];
	*output = *gCurrentInput++;
	*(uint16_t *)output = *(uint16_t *)output << input[1];
}

uint8_t build_table(uint8_t *input, uint16_t *value, uint16_t *index, uint8_t *output)
{
	uint8_t result;
	uint16_t local_index;

	if (check(input, output))
	{
		if ((signed int)++*index < 511)
		{
			local_index = 2 * *index;
			result = build_table(input, value, index, output);
			if (!result)
			{
				gBigTable[local_index] = *value;
				result = build_table(input, value, index, output);
				if (!result)
				{
					gSmallTable[local_index] = *value;
					*value = local_index;
					*value >>= 1;
					result = 0;
				}
			}
		}
		else
		{
			result = 1;
		}
	}
	else
	{
		apply(input, output, value);
		result = 0;
	}

	return result;
}

uint8_t decode_block()
{
	uint8_t result;

	uint32_t i = 0;
	uint32_t stored_i = 0;
	uint16_t index = 0;
	uint8_t  first[4] = { 0,0,0,0 };
	uint8_t  second[4] = { 0,0,0,0 };

	*(uint16_t *)first = *gCurrentInput;
	*(uint32_t *)first = *(uint16_t *)first << 16;
	first[1] = gCurrentInput[1];
	first[0] = gCurrentInput[2];
	gCurrentInput += 3;

	stored_i = *(uint32_t *)first;
	first[1] = 0;
	*(uint16_t *)&second[1] = *gCurrentInput;
	second[0] = gCurrentInput[1];
	gCurrentInput += 2;

	i = 0xFF;
	result = build_table(first, &index, (uint16_t*)&i, second);
	uint16_t original_index = index;
	if (!result)
	{
		i = stored_i;
		do
		{
			index = original_index;
			while (index >= 0x100)
			{
				index *= 2;
				if (check(first, second)) 
				{
					index = gSmallTable[index];
				}
				else {
					index = gBigTable[index];
				}
			}
			*gCurrentOutput++ = (uint8_t)index;
			++gNumDecodedBytes;
			--i;
		} while (i);
		--gCurrentInput;
		result = 0;
		if (!first[1])
			--gCurrentInput;
	}

	return result;
}

uint8_t decompress(uint8_t *input, uint8_t *output)
{
	int result;
	char currentByte;

	// Reset global state
	gNumDecodedBytes = 0;
	gCurrentInput = input;
	gCurrentOutput = output;
	memset(gBigTable, 0, sizeof(gBigTable));
	memset(gSmallTable, 0, sizeof(gSmallTable));

	while (1)
	{
		currentByte = *gCurrentInput++;
		if (currentByte != 1)
			break;
		result = decode_block();
		if (result)
			return result;
	}
	return currentByte != 0;
}

uint8_t comextract(uint8_t* input_buffer, size_t input_size, uint8_t** output_buffer, size_t* output_size) {
	uint8_t* output = NULL;
	size_t size = 0;
	size_t rest = 0;

	if (input_size < TOSHIBA_COM_HEADER_VERSION_0_HEADER_SIZE) {
		return 1;
	}

	for (size_t i = 0; i < input_size - sizeof(TOSHIBA_COM_HEADER); i++) {
		// Search input file for BIOS signature
		if (*(uint32_t*)(input_buffer + i + 3) == TOSHIBA_COM_HEADER_SIGNATURE) {
			size_t header_size;
			size_t compressed_size;
			size_t decompressed_size;
			uint8_t result;
			
			rest = input_size - i + 3;
			if (rest < sizeof(TOSHIBA_COM_HEADER)) {
				break;
			}

			// Map this part of file as a candidate for header
			const TOSHIBA_COM_HEADER* header = (const TOSHIBA_COM_HEADER*)(input_buffer + i);

			// Check first 2 bytes to be zero
			if (header->Zero != 0) {
				continue;
			}
			
			printf("Toshiba COM header candidate found at offset 0x%X\n", (unsigned) i);

			// Determine header size based on header version
			if (header->HeaderVersion == 0) {
				header_size = TOSHIBA_COM_HEADER_VERSION_0_HEADER_SIZE;
			}
			else if (header->HeaderVersion == 2) {
				header_size = TOSHIBA_COM_HEADER_VERSION_2_HEADER_SIZE;
			}
			else {
				printf("Unknown header version 0x%X, assuming header size 0x%X\n", header->HeaderVersion, TOSHIBA_COM_HEADER_VERSION_2_HEADER_SIZE);
				header_size = TOSHIBA_COM_HEADER_VERSION_2_HEADER_SIZE;
			}
			if (rest < header_size + sizeof(uint32_t)) {
				continue;
			}

			// Check sanity of compression byte
			if (header->Compressed > 1) {
				printf("Candidate skipped, compression state is unknown (0x%X)\n", header->Compressed);
				continue;
			}

			// Get data sizes
			compressed_size = header->CompressedSize;
			decompressed_size = ((size_t)header->DecompressedSizeShifted) << 10;

			// Check sanity of both sizes
			if (compressed_size > decompressed_size) {
				printf("Candidate skipped, compressed size is larger than decompressed size\n");
				continue;
			}
			if (decompressed_size > 0x400000) {
				printf("Candidate skipped, decompressed size is larger than 4 Mb\n");
				continue;
			}
			if (rest < header_size + compressed_size) {
				continue;
			}

			// Show BIOS version
			{
				uint8_t version[sizeof(header->BiosVersion) + 1];
				memcpy(version, header->BiosVersion, sizeof(header->BiosVersion));
				version[sizeof(header->BiosVersion)] = 0;
				printf("Toshiba COM header appears valid, BIOS version: %s\n", version);
			}

			// Perform decompression
			if (header->Compressed == 0) {
				printf("File is not compressed, data start is at offset 0x%X\n", (unsigned) (i + header_size));
				return 1;
			}
			else if (header->Compressed == 1) {
				printf("File is compressed, decompressing...\n");

				// (Re)allocate output buffer 
				size += decompressed_size;
				output = (uint8_t*)realloc(output, size);

				// Call decompression fuction
				result = decompress((uint8_t*)header + header_size, output + size - decompressed_size);
				if (result) {
					printf("Decompression failed, bailing\n");
					return 1;
				}
				printf("Decompressed 0x%X bytes\n", (unsigned) decompressed_size);

				// Advance position
				i += header_size + compressed_size;
			}
		}
	}

	if (size == 0) {
		// Nothing was found
		return 1;
	}

	*output_buffer = output;
	*output_size = size;
	return 0;
}

// main
int main(int argc, char* argv[])
{
	FILE*  file;
	uint8_t* buffer;
	uint8_t* image;
	size_t  filesize;
	size_t  imagesize;
	size_t  read;
	uint8_t  status;

	// Check arguments count
	if (argc != 3) {
		// Print usage and exit
		printf("Toshiba COM Extractor v0.1.0 - extracts payload from compressed COM file used in Toshiba BIOS updates\n\n"
		       "Usage: comextract infile.com outfile.bin\n");
		return 7;
	}

	// Read input file
	file = fopen(argv[1], "rb");
	if (!file) {
		printf("Can't open input file\n");
		return 2;
	}

	// Get file size
	fseek(file, 0, SEEK_END);
	filesize = ftell(file);
	fseek(file, 0, SEEK_SET);

	// Allocate buffer
	buffer = (uint8_t*)malloc(filesize);
	if (!buffer) {
		printf("Can't allocate memory for input file\n");
		return 3;
	}

	// Read the whole file into buffer
	read = fread((void*)buffer, 1, filesize, file);
	if (read != filesize) {
		printf("Can't read input file\n");
		return 4;
	}
	
	// Close input file
	fclose(file);

	// Call extraction routine
	status = comextract(buffer, filesize, &image, &imagesize);
	if (status)
		return status;
	
	// Create output file
	file = fopen(argv[2], "wb");
	if (!file) {
		printf("Can't create output file\n");
		return 5;
	}
	
	// Write extracted image
	if (fwrite(image, 1, imagesize, file) != imagesize) {
		printf("Can't write to output file\n");
		return 6;
	}

	// Close output file 
	fclose(file);

	return 0;
}
