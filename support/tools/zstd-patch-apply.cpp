#include "zstd.h"

#include <cstdint>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

std::vector<uint8_t> read_file(const std::string &p_path) {
	std::ifstream input(p_path, std::ios::binary | std::ios::ate);
	if (!input) {
		throw std::runtime_error("open input failed: " + p_path);
	}
	std::streamsize size = input.tellg();
	if (size < 0) {
		throw std::runtime_error("read input size failed: " + p_path);
	}
	input.seekg(0, std::ios::beg);
	std::vector<uint8_t> data(static_cast<size_t>(size));
	if (!data.empty() && !input.read(reinterpret_cast<char *>(data.data()), size)) {
		throw std::runtime_error("read input failed: " + p_path);
	}
	return data;
}

void write_file(const std::string &p_path, const std::vector<uint8_t> &p_data) {
	std::ofstream output(p_path, std::ios::binary | std::ios::trunc);
	if (!output) {
		throw std::runtime_error("open output failed: " + p_path);
	}
	if (!p_data.empty()) {
		output.write(reinterpret_cast<const char *>(p_data.data()),
				static_cast<std::streamsize>(p_data.size()));
	}
	if (!output) {
		throw std::runtime_error("write output failed: " + p_path);
	}
}

int main(int p_argc, char **p_argv) {
	if (p_argc != 4) {
		std::cerr << "usage: zstd-patch-apply OLD PATCH OUTPUT\n";
		return 2;
	}

	try {
		std::vector<uint8_t> dictionary = read_file(p_argv[1]);
		std::vector<uint8_t> patch = read_file(p_argv[2]);
		unsigned long long content_size = ZSTD_getFrameContentSize(patch.data(), patch.size());
		if (content_size == ZSTD_CONTENTSIZE_ERROR || content_size == ZSTD_CONTENTSIZE_UNKNOWN ||
				content_size > std::numeric_limits<size_t>::max()) {
			std::cerr << "patch does not contain a supported output size\n";
			return 3;
		}

		std::vector<uint8_t> output(static_cast<size_t>(content_size));
		ZSTD_DCtx *context = ZSTD_createDCtx();
		if (context == nullptr) {
			std::cerr << "create ZSTD decompression context failed\n";
			return 4;
		}
		size_t result = ZSTD_decompress_usingDict(context, output.data(), output.size(), patch.data(),
				patch.size(), dictionary.data(), dictionary.size());
		ZSTD_freeDCtx(context);
		if (ZSTD_isError(result)) {
			std::cerr << "apply patch failed: " << ZSTD_getErrorName(result) << "\n";
			return 5;
		}
		if (result != output.size()) {
			std::cerr << "unexpected output size: expected=" << output.size() << " actual=" << result << "\n";
			return 6;
		}

		write_file(p_argv[3], output);
		std::cout << "wrote " << output.size() << " bytes\n";
		return 0;
	} catch (const std::exception &error) {
		std::cerr << error.what() << "\n";
		return 7;
	}
}
