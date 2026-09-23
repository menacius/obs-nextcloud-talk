#include "talk-e2ee.hpp"
#include "obs-log.hpp"

#include <olm/olm.h>

#include <QCryptographicHash>
#include <QJsonDocument>
#include <QMessageAuthenticationCode>
#include <QUuid>

#ifdef Q_OS_WIN
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <bcrypt.h>
#else
#include <openssl/evp.h>
#include <openssl/rand.h>
#endif

#include <algorithm>
#include <climits>
#include <cstring>
#include <optional>

namespace nextcloud_talk {
namespace {

constexpr int keyRingSize = 16;
constexpr int ivLength = 12;
constexpr int gcmTagLength = 16;
constexpr int ratchetWindowSize = 8;

QByteArray secureRandom(int size)
{
	if (size <= 0)
		return {};
	QByteArray output(size, Qt::Uninitialized);
#ifdef Q_OS_WIN
	if (BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(output.data()),
			    static_cast<ULONG>(output.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0)
		return {};
#else
	if (RAND_bytes(reinterpret_cast<unsigned char *>(output.data()), size) != 1)
		return {};
#endif
	return output;
}

QByteArray hkdfSha256(const QByteArray &material, const QByteArray &salt, int outputLength)
{
	const QByteArray extracted = QMessageAuthenticationCode::hash(material, salt, QCryptographicHash::Sha256);
	QByteArray output;
	QByteArray previous;
	for (quint8 counter = 1; output.size() < outputLength && counter != 0; ++counter) {
		QByteArray input = previous;
		input.append(static_cast<char>(counter));
		previous = QMessageAuthenticationCode::hash(input, extracted, QCryptographicHash::Sha256);
		output.append(previous);
	}
	output.truncate(outputLength);
	return output;
}

struct OlmSessionState {
	OlmSessionState() : memory(olm_session_size()), session(olm_session(memory.data())) {}
	~OlmSessionState()
	{
		if (session)
			olm_clear_session(session);
	}
	std::vector<std::uint8_t> memory;
	OlmSession *session = nullptr;
};

struct FrameKey {
	QByteArray material;
	QByteArray aesKey;
};

struct RemoteState {
	std::unique_ptr<OlmSessionState> olm;
	QString startMessageId;
	std::array<std::optional<FrameKey>, keyRingSize> keys;
};

std::string keyFor(const QString &value)
{
	const QByteArray utf8 = value.toUtf8();
	return std::string(utf8.constData(), static_cast<std::size_t>(utf8.size()));
}

bool isOlmError(std::size_t result)
{
	return result == olm_error();
}

FrameKey deriveFrameKey(const QByteArray &material)
{
	return FrameKey{material, hkdfSha256(material, QByteArrayLiteral("TalkFrameEncryptionKey"), 16)};
}

} // namespace

class TalkE2ee::Impl {
public:
	Impl()
	{
		accountMemory_.resize(olm_account_size());
		account_ = olm_account(accountMemory_.data());
		const QByteArray random = secureRandom(static_cast<int>(olm_create_account_random_length(account_)));
		if (random.isEmpty() || isOlmError(olm_create_account(account_, const_cast<char *>(random.constData()),
								     static_cast<std::size_t>(random.size())))) {
			ObsLogLine(LOG_ERROR) << "[nextcloud-talk] Could not initialize the Olm E2EE account";
			return;
		}
		const std::size_t identityLength = olm_account_identity_keys_length(account_);
		QByteArray identity(static_cast<qsizetype>(identityLength), Qt::Uninitialized);
		if (isOlmError(olm_account_identity_keys(account_, identity.data(), identityLength)))
			return;
		const QJsonObject keys = QJsonDocument::fromJson(identity).object();
		identityKey_ = keys.value(QStringLiteral("curve25519")).toString();
		localMaterial_ = secureRandom(32);
		localKey_ = deriveFrameKey(localMaterial_);
		ready_ = !identityKey_.isEmpty() && localMaterial_.size() == 32;
	}

	~Impl()
	{
		stop();
		if (account_)
			olm_clear_account(account_);
	}

	void stop()
	{
		std::lock_guard<std::mutex> lock(mutex_);
		remotes_.clear();
		localSessionId_.clear();
		sendCounts_.clear();
		active_ = false;
	}

	QJsonObject encryptedLocalKey(OlmSessionState &state)
	{
		const QJsonObject keyData{{QStringLiteral("key"), QString::fromLatin1(localMaterial_.toBase64())},
					 {QStringLiteral("index"), localKeyIndex_}};
		const QByteArray plaintext = QJsonDocument(keyData).toJson(QJsonDocument::Compact);
		const std::size_t randomLength = olm_encrypt_random_length(state.session);
		QByteArray random = secureRandom(static_cast<int>(randomLength));
		const std::size_t messageLength = olm_encrypt_message_length(state.session, plaintext.size());
		QByteArray message(static_cast<qsizetype>(messageLength), Qt::Uninitialized);
		const std::size_t type = olm_encrypt_message_type(state.session);
		const std::size_t result = olm_encrypt(state.session, plaintext.constData(), plaintext.size(),
						       random.data(), random.size(), message.data(), messageLength);
		if (isOlmError(result)) {
			ObsLogLine(LOG_WARNING) << "[nextcloud-talk] Olm key encryption failed:"
						     << olm_session_last_error(state.session);
			return {};
		}
		message.resize(static_cast<qsizetype>(result));
		return QJsonObject{{QStringLiteral("type"), static_cast<qint64>(type)},
				   {QStringLiteral("body"), QString::fromLatin1(message)}};
	}

	std::optional<QJsonObject> decryptKey(OlmSessionState &state, const QJsonObject &encrypted)
	{
		const std::size_t type = static_cast<std::size_t>(encrypted.value(QStringLiteral("type")).toInteger());
		const QByteArray body = encrypted.value(QStringLiteral("body")).toString().toLatin1();
		if (body.isEmpty())
			return std::nullopt;
		QByteArray probe = body;
		const std::size_t maximum = olm_decrypt_max_plaintext_length(state.session, type, probe.data(), probe.size());
		if (isOlmError(maximum))
			return std::nullopt;
		QByteArray mutableBody = body;
		QByteArray plaintext(static_cast<qsizetype>(maximum), Qt::Uninitialized);
		const std::size_t result = olm_decrypt(state.session, type, mutableBody.data(), mutableBody.size(),
						       plaintext.data(), maximum);
		if (isOlmError(result)) {
			ObsLogLine(LOG_WARNING) << "[nextcloud-talk] Olm key decryption failed:"
						     << olm_session_last_error(state.session);
			return std::nullopt;
		}
		plaintext.resize(static_cast<qsizetype>(result));
		const QJsonDocument document = QJsonDocument::fromJson(plaintext);
		return document.isObject() ? std::optional<QJsonObject>(document.object()) : std::nullopt;
	}

	void storeRemoteKey(RemoteState &remote, const QJsonObject &decoded, const QString &sender)
	{
		if (!decoded.contains(QStringLiteral("key")) || !decoded.contains(QStringLiteral("index")))
			return;
		const QByteArray material = QByteArray::fromBase64(
			decoded.value(QStringLiteral("key")).toString().toLatin1());
		if (material.size() != 32)
			return;
		const int index = decoded.value(QStringLiteral("index")).toInt() % keyRingSize;
		remote.keys[static_cast<std::size_t>((index + keyRingSize) % keyRingSize)] = deriveFrameKey(material);
		ObsLogLine(LOG_INFO) << "[nextcloud-talk] E2EE media key established with" << sender.left(8)
				     << "index=" << index;
	}

	QJsonObject keyPayload(OlmSessionState &session, const QString &type, const QString &id)
	{
		QJsonObject payload{{QStringLiteral("id"), id}, {QStringLiteral("type"), type}};
		payload.insert(QStringLiteral("key"), encryptedLocalKey(session));
		return payload;
	}

	void startSession(const QString &sessionId)
	{
		QJsonObject payload;
		{
			std::lock_guard<std::mutex> lock(mutex_);
			if (!ready_ || sessionId.isEmpty())
				return;
			RemoteState &remote = remotes_[keyFor(sessionId)];
			if (remote.olm || !remote.startMessageId.isEmpty())
				return;
			const std::size_t randomLength = olm_account_generate_one_time_keys_random_length(account_, 1);
			QByteArray random = secureRandom(static_cast<int>(randomLength));
			if (random.isEmpty() || isOlmError(olm_account_generate_one_time_keys(
						 account_, 1, random.data(), random.size())))
				return;
			const std::size_t keysLength = olm_account_one_time_keys_length(account_);
			QByteArray keysJson(static_cast<qsizetype>(keysLength), Qt::Uninitialized);
			if (isOlmError(olm_account_one_time_keys(account_, keysJson.data(), keysLength)))
				return;
			const QJsonObject curveKeys = QJsonDocument::fromJson(keysJson).object()
							 .value(QStringLiteral("curve25519")).toObject();
			if (curveKeys.isEmpty())
				return;
			const QString oneTimeKey = curveKeys.begin().value().toString();
			olm_account_mark_keys_as_published(account_);
			remote.startMessageId = QUuid::createUuid().toString(QUuid::WithoutBraces);
			payload = QJsonObject{{QStringLiteral("id"), remote.startMessageId},
					      {QStringLiteral("type"), QStringLiteral("encryption.start")},
					      {QStringLiteral("identity"), identityKey_},
					      {QStringLiteral("key"), oneTimeKey}};
		}
		if (owner_->sendCallMessage)
			owner_->sendCallMessage(sessionId, payload);
	}

	bool aesGcm(bool encrypt, const FrameKey &key, const QByteArray &iv, const QByteArray &aad,
		    const QByteArray &input, const QByteArray &inputTag, QByteArray &output, QByteArray &outputTag)
	{
#ifdef Q_OS_WIN
		BCRYPT_ALG_HANDLE algorithm = nullptr;
		BCRYPT_KEY_HANDLE keyHandle = nullptr;
		if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_AES_ALGORITHM, nullptr, 0) != 0)
			return false;
		const NTSTATUS modeStatus = BCryptSetProperty(
			algorithm, BCRYPT_CHAINING_MODE, reinterpret_cast<PUCHAR>(const_cast<wchar_t *>(BCRYPT_CHAIN_MODE_GCM)),
			static_cast<ULONG>(sizeof(BCRYPT_CHAIN_MODE_GCM)), 0);
		if (modeStatus != 0 || BCryptGenerateSymmetricKey(
					       algorithm, &keyHandle, nullptr, 0,
					       reinterpret_cast<PUCHAR>(const_cast<char *>(key.aesKey.constData())),
					       static_cast<ULONG>(key.aesKey.size()), 0) != 0) {
			BCryptCloseAlgorithmProvider(algorithm, 0);
			return false;
		}
		output.resize(input.size());
		outputTag = encrypt ? QByteArray(gcmTagLength, 0) : inputTag;
		BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO auth;
		BCRYPT_INIT_AUTH_MODE_INFO(auth);
		auth.pbNonce = reinterpret_cast<PUCHAR>(const_cast<char *>(iv.constData()));
		auth.cbNonce = static_cast<ULONG>(iv.size());
		auth.pbAuthData = reinterpret_cast<PUCHAR>(const_cast<char *>(aad.constData()));
		auth.cbAuthData = static_cast<ULONG>(aad.size());
		auth.pbTag = reinterpret_cast<PUCHAR>(outputTag.data());
		auth.cbTag = static_cast<ULONG>(outputTag.size());
		ULONG written = 0;
		const NTSTATUS status = encrypt
			? BCryptEncrypt(keyHandle, reinterpret_cast<PUCHAR>(const_cast<char *>(input.constData())),
					static_cast<ULONG>(input.size()), &auth, nullptr, 0,
					reinterpret_cast<PUCHAR>(output.data()), static_cast<ULONG>(output.size()), &written, 0)
			: BCryptDecrypt(keyHandle, reinterpret_cast<PUCHAR>(const_cast<char *>(input.constData())),
					static_cast<ULONG>(input.size()), &auth, nullptr, 0,
					reinterpret_cast<PUCHAR>(output.data()), static_cast<ULONG>(output.size()), &written, 0);
		BCryptDestroyKey(keyHandle);
		BCryptCloseAlgorithmProvider(algorithm, 0);
		if (status != 0)
			return false;
		output.resize(static_cast<qsizetype>(written));
		return true;
#else
		if (key.aesKey.size() != 16 || iv.isEmpty() || input.size() > INT_MAX || aad.size() > INT_MAX ||
		    (!encrypt && inputTag.size() != gcmTagLength))
			return false;
		EVP_CIPHER_CTX *context = EVP_CIPHER_CTX_new();
		if (!context)
			return false;
		bool success = false;
		do {
			if (EVP_CipherInit_ex(context, EVP_aes_128_gcm(), nullptr, nullptr, nullptr, encrypt ? 1 : 0) != 1)
				break;
			if (EVP_CIPHER_CTX_ctrl(context, EVP_CTRL_GCM_SET_IVLEN, static_cast<int>(iv.size()), nullptr) != 1)
				break;
			if (EVP_CipherInit_ex(context, nullptr, nullptr,
				      reinterpret_cast<const unsigned char *>(key.aesKey.constData()),
				      reinterpret_cast<const unsigned char *>(iv.constData()), -1) != 1)
				break;
			int written = 0;
			if (!aad.isEmpty() &&
			    EVP_CipherUpdate(context, nullptr, &written,
					     reinterpret_cast<const unsigned char *>(aad.constData()),
					     static_cast<int>(aad.size())) != 1)
				break;
			output.resize(input.size());
			int total = 0;
			if (!input.isEmpty() &&
			    EVP_CipherUpdate(context, reinterpret_cast<unsigned char *>(output.data()), &written,
					     reinterpret_cast<const unsigned char *>(input.constData()),
					     static_cast<int>(input.size())) != 1)
				break;
			total += written;
			if (!encrypt && EVP_CIPHER_CTX_ctrl(
					context, EVP_CTRL_GCM_SET_TAG, gcmTagLength,
					const_cast<char *>(inputTag.constData())) != 1)
				break;
			if (EVP_CipherFinal_ex(context,
					       reinterpret_cast<unsigned char *>(output.data()) + total, &written) != 1)
				break;
			total += written;
			output.resize(total);
			if (encrypt) {
				outputTag.resize(gcmTagLength);
				if (EVP_CIPHER_CTX_ctrl(context, EVP_CTRL_GCM_GET_TAG, gcmTagLength, outputTag.data()) != 1)
					break;
			} else {
				outputTag = inputTag;
			}
			success = true;
		} while (false);
		EVP_CIPHER_CTX_free(context);
		if (!success) {
			output.clear();
			outputTag.clear();
		}
		return success;
#endif
	}

	TalkE2ee *owner_ = nullptr;
	std::mutex mutex_;
	std::vector<std::uint8_t> accountMemory_;
	OlmAccount *account_ = nullptr;
	QString identityKey_;
	QString localSessionId_;
	QByteArray localMaterial_;
	FrameKey localKey_;
	int localKeyIndex_ = 0;
	bool ready_ = false;
	bool active_ = false;
	std::unordered_map<std::string, RemoteState> remotes_;
	std::unordered_map<std::uint32_t, std::uint32_t> sendCounts_;
};

TalkE2ee::TalkE2ee() : impl_(std::make_unique<Impl>())
{
	impl_->owner_ = this;
}

TalkE2ee::~TalkE2ee() = default;

void TalkE2ee::start(const QString &localSessionId)
{
	std::lock_guard<std::mutex> lock(impl_->mutex_);
	impl_->localSessionId_ = localSessionId;
}

void TalkE2ee::stop()
{
	impl_->stop();
}

void TalkE2ee::participantJoined(const QString &sessionId)
{
	QString local;
	{
		std::lock_guard<std::mutex> lock(impl_->mutex_);
		local = impl_->localSessionId_;
	}
	if (!local.isEmpty() && sessionId < local)
		impl_->startSession(sessionId);
}

void TalkE2ee::participantLeft(const QString &sessionId)
{
	std::lock_guard<std::mutex> lock(impl_->mutex_);
	impl_->remotes_.erase(keyFor(sessionId));
}

bool TalkE2ee::handleMessage(const QString &sender, const QJsonObject &message)
{
	if (message.value(QStringLiteral("type")).toString() != QStringLiteral("message"))
		return false;
	const QJsonObject payload = message.value(QStringLiteral("payload")).toObject();
	const QString type = payload.value(QStringLiteral("type")).toString();
	if (!type.startsWith(QStringLiteral("encryption.")))
		return false;

	QJsonObject response;
	{
		std::lock_guard<std::mutex> lock(impl_->mutex_);
		RemoteState &remote = impl_->remotes_[keyFor(sender)];
		if (type == QStringLiteral("encryption.start")) {
			if (remote.olm)
				return true;
			remote.olm = std::make_unique<OlmSessionState>();
			const QByteArray identity = payload.value(QStringLiteral("identity")).toString().toLatin1();
			const QByteArray oneTime = payload.value(QStringLiteral("key")).toString().toLatin1();
			QByteArray random = secureRandom(static_cast<int>(olm_create_outbound_session_random_length(
				remote.olm->session)));
			if (isOlmError(olm_create_outbound_session(
					remote.olm->session, impl_->account_, identity.constData(), identity.size(),
					oneTime.constData(), oneTime.size(), random.data(), random.size()))) {
				remote.olm.reset();
				return true;
			}
			response = impl_->keyPayload(*remote.olm, QStringLiteral("encryption.finish"),
						     payload.value(QStringLiteral("id")).toString());
			impl_->active_ = true;
		} else if (type == QStringLiteral("encryption.finish")) {
			if (remote.olm || payload.value(QStringLiteral("id")).toString() != remote.startMessageId)
				return true;
			remote.olm = std::make_unique<OlmSessionState>();
			const QJsonObject encrypted = payload.value(QStringLiteral("key")).toObject();
			QByteArray body = encrypted.value(QStringLiteral("body")).toString().toLatin1();
			if (isOlmError(olm_create_inbound_session(remote.olm->session, impl_->account_, body.data(),
								       body.size()))) {
				remote.olm.reset();
				return true;
			}
			olm_remove_one_time_keys(impl_->account_, remote.olm->session);
			if (const auto decoded = impl_->decryptKey(*remote.olm, encrypted))
				impl_->storeRemoteKey(remote, *decoded, sender);
			remote.startMessageId.clear();
			response = impl_->keyPayload(*remote.olm, QStringLiteral("encryption.setkey"),
						     QUuid::createUuid().toString(QUuid::WithoutBraces));
			impl_->active_ = true;
		} else if (type == QStringLiteral("encryption.setkey") ||
			   type == QStringLiteral("encryption.gotkey")) {
			if (!remote.olm)
				return true;
			if (const auto decoded = impl_->decryptKey(*remote.olm, payload.value(QStringLiteral("key")).toObject()))
				impl_->storeRemoteKey(remote, *decoded, sender);
			if (type == QStringLiteral("encryption.setkey"))
				response = impl_->keyPayload(*remote.olm, QStringLiteral("encryption.gotkey"),
							     payload.value(QStringLiteral("id")).toString());
		}
	}
	if (!response.isEmpty() && sendCallMessage)
		sendCallMessage(sender, response);
	return true;
}

bool TalkE2ee::encryptFrame(std::vector<std::uint8_t> &frame, bool video, bool keyframe,
			    std::uint32_t ssrc, std::uint32_t timestamp)
{
	const std::size_t headerLength = video ? (keyframe ? 10u : 3u) : 1u;
	if (frame.size() <= headerLength)
		return false;
	std::lock_guard<std::mutex> lock(impl_->mutex_);
	if (!impl_->ready_)
		return false;
	if (!impl_->active_)
		return true;
	QByteArray iv(ivLength, 0);
	auto put32 = [&iv](int offset, std::uint32_t value) {
		iv[offset] = static_cast<char>(value >> 24);
		iv[offset + 1] = static_cast<char>(value >> 16);
		iv[offset + 2] = static_cast<char>(value >> 8);
		iv[offset + 3] = static_cast<char>(value);
	};
	put32(0, ssrc);
	put32(4, timestamp);
	put32(8, impl_->sendCounts_[ssrc]++ % 0xffffu);
	const QByteArray aad(reinterpret_cast<const char *>(frame.data()), static_cast<qsizetype>(headerLength));
	const QByteArray plaintext(reinterpret_cast<const char *>(frame.data() + headerLength),
				   static_cast<qsizetype>(frame.size() - headerLength));
	QByteArray ciphertext;
	QByteArray tag;
	if (!impl_->aesGcm(true, impl_->localKey_, iv, aad, plaintext, {}, ciphertext, tag))
		return false;
	std::vector<std::uint8_t> encrypted;
	encrypted.reserve(headerLength + ciphertext.size() + tag.size() + iv.size() + 2);
	encrypted.insert(encrypted.end(), frame.begin(), frame.begin() + static_cast<std::ptrdiff_t>(headerLength));
	encrypted.insert(encrypted.end(), reinterpret_cast<const std::uint8_t *>(ciphertext.constData()),
			 reinterpret_cast<const std::uint8_t *>(ciphertext.constData() + ciphertext.size()));
	encrypted.insert(encrypted.end(), reinterpret_cast<const std::uint8_t *>(tag.constData()),
			 reinterpret_cast<const std::uint8_t *>(tag.constData() + tag.size()));
	encrypted.insert(encrypted.end(), reinterpret_cast<const std::uint8_t *>(iv.constData()),
			 reinterpret_cast<const std::uint8_t *>(iv.constData() + iv.size()));
	encrypted.push_back(ivLength);
	encrypted.push_back(static_cast<std::uint8_t>(impl_->localKeyIndex_));
	frame = std::move(encrypted);
	return true;
}

bool TalkE2ee::decryptFrame(const QString &sender, std::vector<std::uint8_t> &frame, bool video,
			    bool keyframe)
{
	const std::size_t headerLength = video ? (keyframe ? 10u : 3u) : 1u;
	if (frame.size() < headerLength + gcmTagLength + ivLength + 2 ||
	    (frame[frame.size() - 2] & 0x7f) != ivLength || frame.back() >= keyRingSize)
		return true;
	const std::uint8_t keyIndex = frame.back();
	const std::uint8_t encodedIvLength = frame[frame.size() - 2] & 0x7f;
	if (keyIndex >= keyRingSize || encodedIvLength == 0 ||
	    frame.size() < headerLength + gcmTagLength + encodedIvLength + 2)
		return false;
	std::lock_guard<std::mutex> lock(impl_->mutex_);
	auto found = impl_->remotes_.find(keyFor(sender));
	if (found == impl_->remotes_.end() || !found->second.keys[keyIndex])
		return false;
	const std::size_t ivOffset = frame.size() - encodedIvLength - 2;
	const std::size_t tagOffset = ivOffset - gcmTagLength;
	const QByteArray aad(reinterpret_cast<const char *>(frame.data()), static_cast<qsizetype>(headerLength));
	const QByteArray ciphertext(reinterpret_cast<const char *>(frame.data() + headerLength),
				    static_cast<qsizetype>(tagOffset - headerLength));
	const QByteArray tag(reinterpret_cast<const char *>(frame.data() + tagOffset), gcmTagLength);
	const QByteArray iv(reinterpret_cast<const char *>(frame.data() + ivOffset), encodedIvLength);
	FrameKey candidate = *found->second.keys[keyIndex];
	for (int attempt = 0; attempt <= ratchetWindowSize; ++attempt) {
		QByteArray plaintext;
		QByteArray ignoredTag;
		if (impl_->aesGcm(false, candidate, iv, aad, ciphertext, tag, plaintext, ignoredTag)) {
			std::vector<std::uint8_t> decoded;
			decoded.reserve(headerLength + plaintext.size());
			decoded.insert(decoded.end(), frame.begin(), frame.begin() + static_cast<std::ptrdiff_t>(headerLength));
			decoded.insert(decoded.end(), reinterpret_cast<const std::uint8_t *>(plaintext.constData()),
				       reinterpret_cast<const std::uint8_t *>(plaintext.constData() + plaintext.size()));
			frame = std::move(decoded);
			found->second.keys[keyIndex] = candidate;
			return true;
		}
		candidate = deriveFrameKey(hkdfSha256(candidate.material, QByteArrayLiteral("TalkFrameRatchetKey"), 32));
	}
	return false;
}

} // namespace nextcloud_talk
