#pragma once

#include <QString>

#include <cstddef>
#include <cstdint>
#include <memory>

namespace nextcloud_talk {

enum class IncomingCodec { H264, Vp8, Opus, Unsupported };

class IncomingMediaDecoder final {
public:
	IncomingMediaDecoder();
	~IncomingMediaDecoder();

	IncomingMediaDecoder(const IncomingMediaDecoder &) = delete;
	IncomingMediaDecoder &operator=(const IncomingMediaDecoder &) = delete;

	bool decode(IncomingCodec codec, const std::uint8_t *data, std::size_t size,
		    const QString &participantId, bool resetBeforeDecode = false,
		    std::uint64_t videoTimestampNs = 0, bool screenShare = false);

private:
	class Impl;
	std::unique_ptr<Impl> impl_;
};

} // namespace nextcloud_talk
