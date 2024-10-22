// Copyright 2014-2019 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestRequestJSON.h"

#include "VaRestDefines.h"
#include "VaRestJsonObject.h"
#include "VaRestJsonValue.h"
#include "VaRestLibrary.h"
#include "VaRestSettings.h"

#include "Engine/Engine.h"
#include "Engine/LatentActionManager.h"
#include "Engine/World.h"
#include "Interfaces/IHttpResponse.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Misc/FileHelper.h"

FString UVaRestRequestJSON::DeprecatedResponseString(TEXT("DEPRECATED: Please use GetResponseContentAsString() instead"));

template <class T>
void FVaRestLatentAction<T>::Cancel()
{
	UObject* Obj = Request.Get();
	if (Obj != nullptr)
	{
		((UVaRestRequestJSON*)Obj)->Cancel();
	}
}

UVaRestRequestJSON::UVaRestRequestJSON(const class FObjectInitializer& PCIP)
	: Super(PCIP)
	, BinaryContentType(TEXT("application/octet-stream"))
{
	ContinueAction = nullptr;

	RequestVerb = EVaRestRequestVerb::GET;
	RequestContentType = EVaRestRequestContentType::x_www_form_urlencoded_url;

	ResetData();
}

void UVaRestRequestJSON::SetVerb(EVaRestRequestVerb Verb)
{
	RequestVerb = Verb;
}

void UVaRestRequestJSON::SetCustomVerb(FString Verb)
{
	CustomVerb = Verb;
}

void UVaRestRequestJSON::SetContentType(EVaRestRequestContentType ContentType)
{
	RequestContentType = ContentType;
}

void UVaRestRequestJSON::SetBinaryContentType(const FString& ContentType)
{
	BinaryContentType = ContentType;
}

void UVaRestRequestJSON::SetBinaryRequestContent(const TArray<uint8>& Bytes)
{
	RequestBytes = Bytes;
}

void UVaRestRequestJSON::SetStringRequestContent(const FString& Content)
{
	StringRequestContent = Content;
}

void UVaRestRequestJSON::SetHeader(const FString& HeaderName, const FString& HeaderValue)
{
	RequestHeaders.Add(HeaderName, HeaderValue);
}

//////////////////////////////////////////////////////////////////////////
// Destruction and reset

void UVaRestRequestJSON::ResetData()
{
	ResetRequestData();
	ResetResponseData();
}

void UVaRestRequestJSON::ResetRequestData()
{
	if (RequestJsonObj != nullptr)
	{
		RequestJsonObj->Reset();
	}
	else
	{
		RequestJsonObj = NewObject<UVaRestJsonObject>();
	}

	// See issue #90
	// HttpRequest = FHttpModule::Get().CreateRequest();

	RequestBytes.Empty();
	StringRequestContent.Empty();
}

void UVaRestRequestJSON::ResetResponseData()
{
	if (ResponseJsonObj != nullptr)
	{
		ResponseJsonObj->Reset();
	}
	else
	{
		ResponseJsonObj = NewObject<UVaRestJsonObject>();
	}

	if (ResponseJsonValue != nullptr)
	{
		ResponseJsonValue->Reset();
	}
	else
	{
		ResponseJsonValue = NewObject<UVaRestJsonValue>();
	}

	ResponseHeaders.Empty();
	ResponseCode = -1;
	ResponseSize = 0;

	bIsValidJsonResponse = false;

	// #127 Reset string to deprecated state
	ResponseContent = DeprecatedResponseString;

	ResponseBytes.Empty();
	ResponseContentLength = 0;
}

void UVaRestRequestJSON::Cancel()
{
	ContinueAction = nullptr;

	ResetResponseData();
}

//////////////////////////////////////////////////////////////////////////
// JSON data accessors

UVaRestJsonObject* UVaRestRequestJSON::GetRequestObject() const
{
	check(RequestJsonObj);
	return RequestJsonObj;
}

void UVaRestRequestJSON::SetRequestObject(UVaRestJsonObject* JsonObject)
{
	if (JsonObject == nullptr)
	{
		UE_LOG(LogVaRest, Error, TEXT("%s: Provided JsonObject is nullptr"), *VA_FUNC_LINE);
		return;
	}

	RequestJsonObj = JsonObject;
}

UVaRestJsonObject* UVaRestRequestJSON::GetResponseObject() const
{
	check(ResponseJsonObj);
	return ResponseJsonObj;
}

void UVaRestRequestJSON::SetResponseObject(UVaRestJsonObject* JsonObject)
{
	if (JsonObject == nullptr)
	{
		UE_LOG(LogVaRest, Error, TEXT("%s: Provided JsonObject is nullptr"), *VA_FUNC_LINE);
		return;
	}

	ResponseJsonObj = JsonObject;
}

UVaRestJsonValue* UVaRestRequestJSON::GetResponseValue() const
{
	check(ResponseJsonValue);
	return ResponseJsonValue;
}

///////////////////////////////////////////////////////////////////////////
// Response data access

FString UVaRestRequestJSON::GetURL() const
{
	return HttpRequest->GetURL();
}

EVaRestRequestVerb UVaRestRequestJSON::GetVerb() const
{
	return RequestVerb;
}

EVaRestRequestStatus UVaRestRequestJSON::GetStatus() const
{
	return EVaRestRequestStatus((uint8)HttpRequest->GetStatus());
}

int32 UVaRestRequestJSON::GetResponseCode() const
{
	return ResponseCode;
}

FString UVaRestRequestJSON::GetResponseHeader(const FString& HeaderName)
{
	FString Result;

	FString* Header = ResponseHeaders.Find(HeaderName);
	if (Header != nullptr)
	{
		Result = *Header;
	}

	return Result;
}

TArray<FString> UVaRestRequestJSON::GetAllResponseHeaders() const
{
	TArray<FString> Result;
	for (TMap<FString, FString>::TConstIterator It(ResponseHeaders); It; ++It)
	{
		Result.Add(It.Key() + TEXT(": ") + It.Value());
	}
	return Result;
}

int32 UVaRestRequestJSON::GetResponseContentLength() const
{
	return ResponseContentLength;
}

const TArray<uint8>& UVaRestRequestJSON::GetResponseContent() const
{
	return ResponseBytes;
}

//////////////////////////////////////////////////////////////////////////
// URL processing

void UVaRestRequestJSON::SetURL(const FString& Url)
{
	// Be sure to trim URL because it can break links on iOS
	FString TrimmedUrl = Url;

	TrimmedUrl.TrimStartInline();
	TrimmedUrl.TrimEndInline();

	HttpRequest->SetURL(TrimmedUrl);
}

void UVaRestRequestJSON::ProcessURL(const FString& Url)
{
	SetURL(Url);
	ProcessRequest();
}

void UVaRestRequestJSON::ApplyURL(const FString& Url, UVaRestJsonObject*& Result, UObject* WorldContextObject, FLatentActionInfo LatentInfo)
{
	// Be sure to trim URL because it can break links on iOS
	FString TrimmedUrl = Url;

	TrimmedUrl.TrimStartInline();
	TrimmedUrl.TrimEndInline();

	HttpRequest->SetURL(TrimmedUrl);

	// Prepare latent action
	if (UWorld* World = GEngine->GetWorldFromContextObjectChecked(WorldContextObject))
	{
		FLatentActionManager& LatentActionManager = World->GetLatentActionManager();
		FVaRestLatentAction<UVaRestJsonObject*>* Kont = LatentActionManager.FindExistingAction<FVaRestLatentAction<UVaRestJsonObject*>>(LatentInfo.CallbackTarget, LatentInfo.UUID);

		if (Kont != nullptr)
		{
			Kont->Cancel();
			LatentActionManager.RemoveActionsForObject(LatentInfo.CallbackTarget);
		}

		LatentActionManager.AddNewAction(LatentInfo.CallbackTarget, LatentInfo.UUID, ContinueAction = new FVaRestLatentAction<UVaRestJsonObject*>(this, Result, LatentInfo));
	}

	ProcessRequest();
}

void UVaRestRequestJSON::LogRequestContent(const TArray<uint8>& Content)
{
	FString ContentStr;
	for (uint8 Byte : Content)
	{
		if (Byte >= 32 && Byte <= 126) // 可打印字符
		{
			ContentStr.AppendChar((TCHAR)Byte);
		}
		else if (Byte == '\r')
		{
			ContentStr.Append(TEXT("\\r"));
		}
		else if (Byte == '\n')
		{
			ContentStr.Append(TEXT("\\n"));
		}
		else
		{
			ContentStr.Append(FString::Printf(TEXT("[%02X]"), Byte));
		}
	}

	UE_LOG(LogVaRest, Warning, TEXT("Request Content:\n%s"), *ContentStr);
}

void UVaRestRequestJSON::ExecuteProcessRequest()
{
	if (HttpRequest->GetURL().Len() == 0)
	{
		UE_LOG(LogVaRest, Error, TEXT("Request execution attempt with empty URL"));
		return;
	}

	ProcessRequest();
}

void UVaRestRequestJSON::ProcessRequest()
{
	// Force add to root once request is launched
	AddToRoot();

	// Set verb
	switch (RequestVerb)
	{
	case EVaRestRequestVerb::GET:
		HttpRequest->SetVerb(TEXT("GET"));
		break;

	case EVaRestRequestVerb::POST:
		HttpRequest->SetVerb(TEXT("POST"));
		break;

	case EVaRestRequestVerb::PUT:
		HttpRequest->SetVerb(TEXT("PUT"));
		break;

	case EVaRestRequestVerb::DEL:
		HttpRequest->SetVerb(TEXT("DELETE"));
		break;

	case EVaRestRequestVerb::CUSTOM:
		HttpRequest->SetVerb(CustomVerb);
		break;

	default:
		break;
	}

	// Set content-type
	switch (RequestContentType)
	{
	case EVaRestRequestContentType::form_data:
	{
		// 1. 创建 HTTP 请求体边界
		FString Boundary = UVaRestLibrary::GenerateUniqueBoundary();

		HttpRequest->SetHeader(TEXT("Content-Type"), FString::Printf(TEXT("multipart/form-data; boundary=%s"), *Boundary));
	
		UE_LOG(LogVaRest, Log, TEXT("Request (form_data): %s %s"), *HttpRequest->GetVerb(), *HttpRequest->GetURL());

		// 2. 创建 TArray<uint8> 存储最终请求体数据
		TArray<uint8> FinalRequestContent;

		// 3. 处理 JSON 对象中的字段
		if (RequestJsonObj)
		{
			// 获取 "files" 数组并保存为TArray
			const TArray<UVaRestJsonValue*>& FilesArray = RequestJsonObj->GetArrayField("files");
			const int32 TotalFiles = FilesArray.Num();

			UE_LOG(LogVaRest, Warning, TEXT("Total files count: %d"), TotalFiles);

			// 使用.操作符来访问数组
			for (int32 i = 0; i < FilesArray.Num(); ++i)
			{
				// 从数组元素获取JsonObject
				UVaRestJsonValue* JsonValue = FilesArray[i];
				if (!JsonValue)
					//UE_LOG(LogVaRest, Error, TEXT("Invalid JsonValue at index [%d]"), i);
					continue;

				UVaRestJsonObject* FileObject = JsonValue->AsObject();
				if (!FileObject)
					//UE_LOG(LogVaRest, Error, TEXT("Invalid FileObject at index [%d]"), i);
					continue;

				// 提取filepath字段
				FString FilePath = FileObject->GetStringField("filepath");

				// 加载文件数据
				TArray<uint8> FileData;
				if (!FFileHelper::LoadFileToArray(FileData, *FilePath))
				{
					UE_LOG(LogVaRest, Error, TEXT("Failed to load file: %s"), *FilePath);
					continue;
				}

				const FString FileName = FPaths::GetCleanFilename(FilePath);
				const int32 FileSizeKB = FileData.Num() / 1024;
				UE_LOG(LogVaRest, Warning, TEXT("[%d/%d] Processing file: %s (Size: %d KB)"),
					i + 1, TotalFiles, *FileName, FileSizeKB);

				// 构建multipart/form-data请求体

				const FString BeginBoundary = (i == 0) ? 
					FString::Printf(TEXT("--%s\r\n"), *Boundary) :
					FString::Printf(TEXT("\r\n--%s\r\n"), *Boundary);

				const FString ContentDisposition = "Content-Disposition: form-data; name=\"files\"; filename=\"" + FileName + "\"\r\n";

				FString ContentType = "Content-Type: application/octet-stream\r\n\r\n";

				// 按顺序添加所有部分到最终请求体
				FinalRequestContent.Append((uint8*)TCHAR_TO_UTF8(*BeginBoundary), BeginBoundary.Len());
				FinalRequestContent.Append((uint8*)TCHAR_TO_UTF8(*ContentDisposition), ContentDisposition.Len());
				FinalRequestContent.Append((uint8*)TCHAR_TO_UTF8(*ContentType), ContentType.Len());
				FinalRequestContent.Append(FileData);
			}

			// 添加最终结束边界
			FString FinalEndBoundary = "\r\n--" + Boundary + "--\r\n";

			FinalRequestContent.Append((uint8*)TCHAR_TO_UTF8(*FinalEndBoundary), FinalEndBoundary.Len());

			// 设置请求内容
			HttpRequest->SetContent(FinalRequestContent);
		}
		break;
	};
	case EVaRestRequestContentType::x_www_form_urlencoded_url:
	{
		HttpRequest->SetHeader(TEXT("Content-Type"), TEXT("application/x-www-form-urlencoded"));

		FString UrlParams = "";
		uint16 ParamIdx = 0;

		// Loop through all the values and prepare additional url part
		for (auto RequestIt = RequestJsonObj->GetRootObject()->Values.CreateIterator(); RequestIt; ++RequestIt)
		{
			FString Key = RequestIt.Key();
			FString Value = RequestIt.Value().Get()->AsString();

			if (!Key.IsEmpty() && !Value.IsEmpty())
			{
				UrlParams += ParamIdx == 0 ? "?" : "&";
				UrlParams += UVaRestLibrary::PercentEncode(Key) + "=" + UVaRestLibrary::PercentEncode(Value);
			}

			ParamIdx++;
		}

		// Apply params
		HttpRequest->SetURL(HttpRequest->GetURL() + UrlParams);

		// Add optional string content
		if (!StringRequestContent.IsEmpty())
		{
			HttpRequest->SetContentAsString(StringRequestContent);
		}

		// Check extended log to avoid security vulnerability (#133)
		if (UVaRestLibrary::GetVaRestSettings()->bExtendedLog)
		{
			UE_LOG(LogVaRest, Log, TEXT("%s: Request (urlencoded): %s %s %s %s"), *VA_FUNC_LINE, *HttpRequest->GetVerb(), *HttpRequest->GetURL(), *UrlParams, *StringRequestContent);
		}
		else
		{
			UE_LOG(LogVaRest, Log, TEXT("%s: Request (urlencoded): %s %s (check bExtendedLog for additional data)"), *VA_FUNC_LINE, *HttpRequest->GetVerb(), *HttpRequest->GetURL());
		}

		break;
	}
	case EVaRestRequestContentType::x_www_form_urlencoded_body:
	{
		HttpRequest->SetHeader(TEXT("Content-Type"), TEXT("application/x-www-form-urlencoded"));

		FString UrlParams = "";
		uint16 ParamIdx = 0;

		// Add optional string content
		if (!StringRequestContent.IsEmpty())
		{
			UrlParams = StringRequestContent;
		}
		else
		{
			// Loop through all the values and prepare additional url part
			for (auto RequestIt = RequestJsonObj->GetRootObject()->Values.CreateIterator(); RequestIt; ++RequestIt)
			{
				FString Key = RequestIt.Key();
				FString Value = RequestIt.Value().Get()->AsString();

				if (!Key.IsEmpty() && !Value.IsEmpty())
				{
					UrlParams += ParamIdx == 0 ? "" : "&";
					UrlParams += UVaRestLibrary::PercentEncode(Key) + "=" + UVaRestLibrary::PercentEncode(Value);
				}

				ParamIdx++;
			}
		}

		// Apply params
		HttpRequest->SetContentAsString(UrlParams);

		// Check extended log to avoid security vulnerability (#133)
		if (UVaRestLibrary::GetVaRestSettings()->bExtendedLog)
		{
			UE_LOG(LogVaRest, Log, TEXT("%s: Request (url body): %s %s %s"), *VA_FUNC_LINE, *HttpRequest->GetVerb(), *HttpRequest->GetURL(), *UrlParams);
		}
		else
		{
			UE_LOG(LogVaRest, Log, TEXT("%s: Request (url body): %s %s (check bExtendedLog for additional data)"), *VA_FUNC_LINE, *HttpRequest->GetVerb(), *HttpRequest->GetURL());
		}

		break;
	}
	case EVaRestRequestContentType::binary:
	{
		HttpRequest->SetHeader(TEXT("Content-Type"), BinaryContentType);
		HttpRequest->SetContent(RequestBytes);

		UE_LOG(LogVaRest, Log, TEXT("Request (binary): %s %s"), *HttpRequest->GetVerb(), *HttpRequest->GetURL());

		break;
	}
	case EVaRestRequestContentType::json:
	{
		HttpRequest->SetHeader(TEXT("Content-Type"), TEXT("application/json"));

		// Body is ignored for get requests, so we shouldn't place json into it even if it's empty
		if (RequestVerb == EVaRestRequestVerb::GET)
		{
			break;
		}

		// Serialize data to json string
		FString OutputString;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutputString);
		FJsonSerializer::Serialize(RequestJsonObj->GetRootObject(), Writer);

		// Set Json content
		HttpRequest->SetContentAsString(OutputString);

		if (UVaRestLibrary::GetVaRestSettings()->bExtendedLog)
		{
			UE_LOG(LogVaRest, Log, TEXT("Request (json): %s %s %sJSON(%s%s%s)JSON"), *HttpRequest->GetVerb(), *HttpRequest->GetURL(), LINE_TERMINATOR, LINE_TERMINATOR, *OutputString, LINE_TERMINATOR);
		}
		else
		{
			UE_LOG(LogVaRest, Log, TEXT("Request (json): %s %s (check bExtendedLog for additional data)"), *HttpRequest->GetVerb(), *HttpRequest->GetURL());
		}

		break;
	}

	default:
		break;
	}

	// Apply additional headers
	for (TMap<FString, FString>::TConstIterator It(RequestHeaders); It; ++It)
	{
		HttpRequest->SetHeader(It.Key(), It.Value());
	}

	// Bind event
	HttpRequest->OnProcessRequestComplete().BindUObject(this, &UVaRestRequestJSON::OnProcessRequestComplete);

	// Execute the request
	HttpRequest->ProcessRequest();
}

//////////////////////////////////////////////////////////////////////////
// Request callbacks

void UVaRestRequestJSON::OnProcessRequestComplete(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bWasSuccessful)
{
	// Remove from root on completion
	RemoveFromRoot();

	// Be sure that we have no data from previous response
	ResetResponseData();

	// Check we have a response and save response code as int32
	if (Response.IsValid())
	{
		ResponseCode = Response->GetResponseCode();
	}

	// Check we have result to process futher
	if (!bWasSuccessful || !Response.IsValid())
	{
		UE_LOG(LogVaRest, Error, TEXT("Request failed (%d): %s"), ResponseCode, *Request->GetURL());

		// Broadcast the result event
		OnRequestFail.Broadcast(this);
		OnStaticRequestFail.Broadcast(this);

		return;
	}

#if PLATFORM_DESKTOP
	// Log response state
	UE_LOG(LogVaRest, Log, TEXT("Response (%d): %sJSON(%s%s%s)JSON"), ResponseCode, LINE_TERMINATOR, LINE_TERMINATOR, *Response->GetContentAsString(), LINE_TERMINATOR);
#endif

	// Process response headers
	TArray<FString> Headers = Response->GetAllHeaders();
	for (FString Header : Headers)
	{
		FString Key;
		FString Value;
		if (Header.Split(TEXT(": "), &Key, &Value))
		{
			ResponseHeaders.Add(Key, Value);
		}
	}

	if (UVaRestLibrary::GetVaRestSettings()->bUseChunkedParser)
	{
		// Try to deserialize data to JSON
		const TArray<uint8>& Bytes = Response->GetContent();
		ResponseSize = ResponseJsonObj->DeserializeFromUTF8Bytes((const ANSICHAR*)Bytes.GetData(), Bytes.Num());

		// Log errors
		if (ResponseSize == 0)
		{
			// As we assume it's recommended way to use current class, but not the only one,
			// it will be the warning instead of error
			UE_LOG(LogVaRest, Warning, TEXT("JSON could not be decoded!"));
		}
	}
	else
	{
		// Use default unreal one
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(*Response->GetContentAsString());
		TSharedPtr<FJsonValue> OutJsonValue;
		if (FJsonSerializer::Deserialize(Reader, OutJsonValue))
		{
			ResponseJsonValue->SetRootValue(OutJsonValue);

			if (ResponseJsonValue->GetType() == EVaJson::Object)
			{
				ResponseJsonObj->SetRootObject(ResponseJsonValue->GetRootValue()->AsObject());
				ResponseSize = Response->GetContentLength();
			}
		}
	}

	// Decide whether the request was successful
	bIsValidJsonResponse = bWasSuccessful && (ResponseSize > 0);

	if (!bIsValidJsonResponse)
	{
		// Save response data as a string
		ResponseContent = Response->GetContentAsString();
		ResponseSize = ResponseContent.GetAllocatedSize();

		ResponseBytes = Response->GetContent();
		ResponseContentLength = Response->GetContentLength();
	}

	// Broadcast the result events on next tick
	OnRequestComplete.Broadcast(this);
	OnStaticRequestComplete.Broadcast(this);

	// Finish the latent action
	if (ContinueAction)
	{
		FVaRestLatentAction<UVaRestJsonObject*>* K = ContinueAction;
		ContinueAction = nullptr;

		K->Call(ResponseJsonObj);
	}
}

//////////////////////////////////////////////////////////////////////////
// Tags

void UVaRestRequestJSON::AddTag(FName Tag)
{
	if (Tag != NAME_None)
	{
		Tags.AddUnique(Tag);
	}
}

int32 UVaRestRequestJSON::RemoveTag(FName Tag)
{
	return Tags.Remove(Tag);
}

bool UVaRestRequestJSON::HasTag(FName Tag) const
{
	return (Tag != NAME_None) && Tags.Contains(Tag);
}

//////////////////////////////////////////////////////////////////////////
// Data

FString UVaRestRequestJSON::GetResponseContentAsString(bool bCacheResponseContent)
{
	// Check we have valid json response
	if (!bIsValidJsonResponse)
	{
		// We've cached response content in OnProcessRequestComplete()
		return ResponseContent;
	}

	// Check we have valid response object
	if (!ResponseJsonObj || !ResponseJsonObj->IsValidLowLevel())
	{
		// Discard previous cached string if we had one
		ResponseContent = DeprecatedResponseString;

		return TEXT("Invalid response");
	}

	// Check if we should re-genetate it in runtime
	if (!bCacheResponseContent)
	{
		UE_LOG(LogVaRest, Warning, TEXT("%s: Use of uncashed getter could be slow"), *VA_FUNC_LINE);
		return ResponseJsonObj->EncodeJson();
	}

	// Check that we haven't cached content yet
	if (ResponseContent == DeprecatedResponseString)
	{
		UE_LOG(LogVaRest, Warning, TEXT("%s: Response content string is cached"), *VA_FUNC_LINE);
		ResponseContent = ResponseJsonObj->EncodeJson();
	}

	// Return previously cached content now
	return ResponseContent;
}
