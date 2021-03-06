#include "Download.h"
#include "Log.h"
#include "MUtil.h"
#include "MFile.h"
#include "Hash.h"

#include "sodium.h"

// Include MWindows.h to undefine all the Windows macros that curl.h brought in.
#include "MWindows.h"
#include "curl/curl.h"

// MInetUtil.h must be included after curl.h, otherwise some Winsock stuff errors out.
#include "MInetUtil.h"

static int GetCurlResponseCode(CURL* curl)
{
	long ResponseCode;
	auto curl_ret = curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &ResponseCode);
	if (curl_ret != CURLE_OK)
	{
		Log.Error("curl_easy_getinfo returned error code %d, error message: %s, "
			"when trying to retrieve the response code\n",
			int(curl_ret), curl_easy_strerror(curl_ret));
		assert(false);
		return -1;
	}

	return int(ResponseCode);
}

enum class ProtocolType
{
	HTTP,
	HTTPS,
	FILE,
	Unknown,
};

struct DownloadInfoContext
{
	void* curl;
	const char* Range;
	ProtocolType Protocol;
};

static auto&& GetContext(void* Data)
{
	return *static_cast<DownloadInfoContext*>(Data);
}

bool DownloadInfo::IsRange()
{
	auto&& Ctx = GetContext(Data);

	switch (Ctx.Protocol)
	{
	case ProtocolType::FILE:
		// FILE always returns a range if you requested one.
		return Ctx.Range != nullptr;

	case ProtocolType::HTTP:
	case ProtocolType::HTTPS:
		// The server should return HTTP 206 Partial Content if it's a range.
		return GetCurlResponseCode(Ctx.curl) == 206;

	default:
		Log.Error("DownloadInfo::IsRange -- Got protocol %d, but not sure what to do with it\n",
			Ctx.Protocol);
		return false;
	}
}

optional<double> DownloadInfo::ContentLength()
{
	double ContentLength;
	auto curl_ret = curl_easy_getinfo(Data, CURLINFO_CONTENT_LENGTH_DOWNLOAD, &ContentLength);
	if (curl_ret != CURLE_OK)
	{
		Log.Error("curl_easy_getinfo returned error code %d, error message: %s, "
			"when trying to retrieve the download content length\n",
			int(curl_ret), curl_easy_strerror(curl_ret));
		assert(false);
		return nullopt;
	}

	if (ContentLength == -1)
		return nullopt;

	return ContentLength;
}

static CURL* CreateCurl()
{
	auto res = curl_global_init(CURL_GLOBAL_DEFAULT);
	if (res != CURLE_OK)
		return false;
	return curl_easy_init();
}

static void DestroyCurl(CURL* curl)
{
	if (curl)
		curl_easy_cleanup(curl);
	curl_global_cleanup();
}

DownloadManagerType CreateDownloadManager()
{
	return DownloadManagerType{ CreateCurl() };
}

void DownloadManagerDeleter::operator()(void* curl) const
{
	DestroyCurl(curl);
}

struct CurlWriteData
{
	CURL* curl;

	const std::function<DownloadCallbackType>& Callback;

	// Null if no hash was requested.
	crypto_generichash_blake2b_state* HashState;

	// Null if no size was requested.
	u64* SizeOutput;

	DownloadInfoContext Context;
};

extern "C" static size_t CurlWriteFunction(void* buffer, size_t size, size_t nmemb, void* stream)
{
	assert(stream != nullptr);

	auto& Data = *static_cast<CurlWriteData*>(stream);

	const auto total_size = size * nmemb;

	if (Data.HashState)
	{
		crypto_generichash_blake2b_update(Data.HashState,
			reinterpret_cast<const unsigned char*>(buffer), total_size);
	}

	if (Data.SizeOutput)
	{
		*Data.SizeOutput += total_size;
	}

	DownloadInfo Info{ &Data.Context };
	auto CallbackRet = Data.Callback(static_cast<const u8*>(buffer), total_size, Info);

	return CallbackRet ? total_size : 0;
}

static bool SetProtocol(ProtocolType& Protocol, const char* URL)
{
	auto URLView = StringView{ URL };
	auto ColonIndex = URLView.find_first_of(':');
	if (ColonIndex == URLView.npos)
		return false;

	auto ProtocolString = URLView.substr(0, ColonIndex);

	if (ProtocolString == "http")
		Protocol = ProtocolType::HTTP;
	else if (ProtocolString == "https")
		Protocol = ProtocolType::HTTPS;
	else if (ProtocolString == "file")
		Protocol = ProtocolType::FILE;
	else
		return false;

	return true;
}

bool DownloadFile(const DownloadManagerType& DownloadManager,
	const char* URL,
	int Port,
	const std::function<DownloadCallbackType>& Callback,
	const char* Range,
	Hash::Strong* HashOutput,
	u64* SizeOutput)
{
	crypto_generichash_blake2b_state state;
	if (HashOutput)
	{
		crypto_generichash_blake2b_init(&state,
			nullptr, 0, // Key
			sizeof(HashOutput->Value));
	}

	if (SizeOutput)
		*SizeOutput = 0;

	const auto curl = DownloadManager.get();

	auto* pstate = HashOutput != nullptr ? &state : nullptr;
	CurlWriteData WriteData{ curl, Callback, pstate, SizeOutput };
	WriteData.Context.curl = curl;
	WriteData.Context.Range = Range;
	if (!SetProtocol(WriteData.Context.Protocol, URL))
	{
		Log.Error("Unrecognized protocol in URL %s\n", URL);
		return false;
	}

	if (!curl)
	{
		Log.Fatal("curl is dead! can't download files\n");
		return false;
	}

	// Log and return false if curl_easy_setopt returns an error.
#define curl_easy_setopt_v(...)\
	do {\
		const auto res = curl_easy_setopt(__VA_ARGS__);\
		if (res != CURLE_OK) {\
			Log.Error("curl_easy_setopt(" #__VA_ARGS__ ") returned %d\n", res);\
			assert(false);\
			return false;\
		}\
	} while (false);

	curl_easy_setopt_v(curl, CURLOPT_URL, URL);
	curl_easy_setopt_v(curl, CURLOPT_PORT, Port);
	curl_easy_setopt_v(curl, CURLOPT_WRITEFUNCTION, CurlWriteFunction);
	curl_easy_setopt_v(curl, CURLOPT_WRITEDATA, &WriteData);
	curl_easy_setopt_v(curl, CURLOPT_RANGE, Range);
	// Make curl return CURLE_HTTP_RETURNED_ERROR on HTTP response >= 400.
	curl_easy_setopt_v(curl, CURLOPT_FAILONERROR, 1l);

	const auto res = curl_easy_perform(curl);

	if (HashOutput)
	{
		crypto_generichash_blake2b_final(&state, HashOutput->Value, sizeof(HashOutput->Value));
	}

	if (res != CURLE_OK)
	{
		if (res == CURLE_HTTP_RETURNED_ERROR)
		{
			const int ResponseCode = GetCurlResponseCode(curl);
			Log.Error("Received HTTP error code %d when trying to download from URL %s\n",
				ResponseCode, URL);
		}
		else
		{
			Log.Error("Curl error! Code = %d, message = \"%s\".\n",
				res, curl_easy_strerror(res));
		}

		return false;
	}

	return true;
}

bool DownloadFileToFile(const DownloadManagerType& DownloadManager,
	const char* URL,
	int Port,
	const char* OutputPath,
	const char* Range,
	Hash::Strong* HashOutput,
	u64* SizeOutput)
{
	MFile::RWFile File{ OutputPath, MFile::ClearExistingContents };
	if (File.error())
	{
		Log(LogLevel::Error, "Couldn't open file %s for writing\n", OutputPath);
		return false;
	}

	const auto Callback = [&](const void* Buffer, size_t Size, DownloadInfo&)
	{
		auto ret = File.write(Buffer, Size);
		return ret == Size;
	};
	
	const auto DownloadSuccess = DownloadFile(DownloadManager,
		URL,
		Port,
		std::ref(Callback),
		nullptr,
		HashOutput,
		SizeOutput);

	const auto WriteSuccess = !File.error();

	return DownloadSuccess && WriteSuccess;
}