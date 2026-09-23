#include <rtc/rtc.hpp>

#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

bool waitFor(std::condition_variable &condition, std::unique_lock<std::mutex> &lock,
	     const std::function<bool()> &predicate)
{
	return condition.wait_for(lock, std::chrono::seconds(3), predicate);
}

} // namespace

int main()
{
	rtc::InitLogger(rtc::LogLevel::None);

	std::mutex mutex;
	std::condition_variable condition;
	std::string offerSdp;
	auto publisher = std::make_shared<rtc::PeerConnection>();
	publisher->onLocalDescription([&](rtc::Description description) {
		if (description.type() != rtc::Description::Type::Offer)
			return;
		{
			std::lock_guard<std::mutex> lock(mutex);
			offerSdp = static_cast<std::string>(description);
		}
		condition.notify_all();
	});
	auto publisherStatus = publisher->createDataChannel("status");
	publisher->setLocalDescription();

	{
		std::unique_lock<std::mutex> lock(mutex);
		if (!waitFor(condition, lock, [&] { return !offerSdp.empty(); })) {
			std::cerr << "FAIL: publisher offer was not generated\n";
			return 1;
		}
	}

	std::vector<rtc::Description::Type> subscriberDescriptions;
	auto subscriber = std::make_shared<rtc::PeerConnection>();
	subscriber->onLocalDescription([&](rtc::Description description) {
		{
			std::lock_guard<std::mutex> lock(mutex);
			subscriberDescriptions.push_back(description.type());
		}
		condition.notify_all();
	});

	// This is the plugin's subscriber sequence: first accept the remote offer
	// containing the application m-line, then create the Talk status channel.
	subscriber->setRemoteDescription(rtc::Description(offerSdp, "offer"));
	auto subscriberStatus = subscriber->createDataChannel("status");

	{
		std::unique_lock<std::mutex> lock(mutex);
		if (!waitFor(condition, lock, [&] { return !subscriberDescriptions.empty(); })) {
			std::cerr << "FAIL: subscriber answer was not generated\n";
			return 1;
		}
	}
	std::this_thread::sleep_for(std::chrono::milliseconds(200));

	{
		std::lock_guard<std::mutex> lock(mutex);
		if (subscriberDescriptions.size() != 1 ||
		    subscriberDescriptions.front() != rtc::Description::Type::Answer) {
			std::cerr << "FAIL: creating the receiver status channel caused offer glare\n";
			return 1;
		}
	}

	subscriberStatus->close();
	publisherStatus->close();
	subscriber->close();
	publisher->close();
	std::cout << "Data channel negotiation test passed\n";
	return 0;
}
