/***************************************************************
 *
 * Copyright (C) 2024, Pelican Project, Morgridge Institute for Research
 *
 * Licensed under the Apache License, Version 2.0 (the "License"); you
 * may not use this file except in compliance with the License.  You may
 * obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 ***************************************************************/

#include "S3File.hh"
#include "CurlWorker.hh"
#include "S3Commands.hh"
#include "S3FileSystem.hh"
#include "logging.hh"
#include "stl_string_utils.hh"

#include <XrdOuc/XrdOucEnv.hh>
#include <XrdOuc/XrdOucStream.hh>
#include <XrdSec/XrdSecEntity.hh>
#include <XrdSec/XrdSecEntityAttr.hh>
#include <XrdSfs/XrdSfsInterface.hh>
#include <XrdVersion.hh>

#include <curl/curl.h>

#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdlib.h>
#include <string>
#include <vector>

using namespace XrdHTTPServer;

S3FileSystem *g_s3_oss = nullptr;

XrdVERSIONINFO(XrdOssGetFileSystem, S3);

S3File::S3File(XrdSysError &log, S3FileSystem *oss)
	: m_log(log), m_oss(oss), content_length(0), last_modified(0),
	  write_buffer(""), partNumber(1) {}

int S3File::Open(const char *path, int Oflag, mode_t Mode, XrdOucEnv &env) {
	if (Oflag & O_CREAT) {
		m_log.Log(LogMask::Info, "File opened for creation: ", path);
	}
	if (Oflag & O_APPEND) {
		m_log.Log(LogMask::Info, "File opened for append: ", path);
	}

	if (m_log.getMsgMask() & XrdHTTPServer::Debug) {
		m_log.Log(LogMask::Warning, "S3File::Open", "Opening file", path);
	}

	std::string exposedPath, object;
	auto rv = m_oss->parsePath(path, exposedPath, object);
	if (rv != 0) {
		return rv;
	}
	auto ai = m_oss->getS3AccessInfo(exposedPath, object);
	if (!ai) {
		return -ENOENT;
	}
	if (ai->getS3BucketName().empty()) {
		return -EINVAL;
	}

	m_ai = *ai;
	m_object = object;

	// This flag is not set when it's going to be a read operation
	// so we check if the file exists in order to be able to return a 404
	if (!Oflag) {
		AmazonS3Head head(m_ai, m_object, m_log);

		if (!head.SendRequest()) {
			return -ENOENT;
		}
	}

	return 0;
}

ssize_t S3File::Read(void *buffer, off_t offset, size_t size) {
	AmazonS3Download download(m_ai, m_object, m_log);

	if (!download.SendRequest(offset, size)) {
		std::stringstream ss;
		ss << "Failed to send GetObject command: " << download.getResponseCode()
		   << "'" << download.getResultString() << "'";
		m_log.Log(LogMask::Warning, "S3File::Read", ss.str().c_str());
		return 0;
	}

	const std::string &bytes = download.getResultString();
	memcpy(buffer, bytes.data(), bytes.size());
	return bytes.size();
}

int S3File::Fstat(struct stat *buff) {
	AmazonS3Head head(m_ai, m_object, m_log);

	if (!head.SendRequest()) {
		auto httpCode = head.getResponseCode();
		if (httpCode) {
			std::stringstream ss;
			ss << "HEAD command failed: " << head.getResponseCode() << ": "
			   << head.getResultString();
			m_log.Log(LogMask::Warning, "S3ile::Fstat", ss.str().c_str());
			switch (httpCode) {
			case 404:
				return -ENOENT;
			case 500:
				return -EIO;
			case 403:
				return -EPERM;
			default:
				return -EIO;
			}
		} else {
			std::stringstream ss;
			ss << "Failed to send HEAD command: " << head.getErrorCode() << ": "
			   << head.getErrorMessage();
			m_log.Log(LogMask::Warning, "S3File::Fstat", ss.str().c_str());
			return -EIO;
		}
	}

	std::string headers = head.getResultString();

	std::string line;
	size_t current_newline = 0;
	size_t next_newline = std::string::npos;
	size_t last_character = headers.size();
	while (current_newline != std::string::npos &&
		   current_newline != last_character - 1) {
		next_newline = headers.find("\r\n", current_newline + 2);
		line = substring(headers, current_newline + 2, next_newline);

		size_t colon = line.find(":");
		if (colon != std::string::npos && colon != line.size()) {
			std::string attr = substring(line, 0, colon);
			std::string value = substring(line, colon + 1);
			trim(value);
			toLower(attr);

			if (attr == "content-length") {
				this->content_length = std::stol(value);
			} else if (attr == "last-modified") {
				struct tm t;
				char *eos = strptime(value.c_str(), "%a, %d %b %Y %T %Z", &t);
				if (eos == &value.c_str()[value.size()]) {
					time_t epoch = timegm(&t);
					if (epoch != -1) {
						this->last_modified = epoch;
					}
				}
			}
		}

		current_newline = next_newline;
	}

	memset(buff, '\0', sizeof(struct stat));
	buff->st_mode = 0600 | S_IFREG;
	buff->st_nlink = 1;
	buff->st_uid = 1;
	buff->st_gid = 1;
	buff->st_size = this->content_length;
	buff->st_mtime = this->last_modified;
	buff->st_atime = 0;
	buff->st_ctime = 0;
	buff->st_dev = 0;
	buff->st_ino = 0;

	return 0;
}

ssize_t S3File::Write(const void *buffer, off_t offset, size_t size) {
	if (uploadId == "") {
		AmazonS3CreateMultipartUpload startUpload(m_ai, m_object, m_log);
		if (!startUpload.SendRequest()) {
			m_log.Emsg("Open", "S3 multipart request failed");
			return -ENOENT;
		}
		std::string errMsg;
		startUpload.Results(uploadId, errMsg);
	}

	std::string payload((char *)buffer, size);
	size_t payload_size = payload.length();
	if (payload_size != size) {
		return -ENOENT;
	}
	write_buffer += payload;

	// XXX should this be configurable? 100mb gives us a TB of file size. It
	// doesn't seem terribly useful to be much smaller and it's not clear the S3
	// API will work if it's much larger.
	if (write_buffer.length() > 100000000) {
		if (SendPart() == -ENOENT) {
			return -ENOENT;
		}
	}
	return size;
}

int S3File::SendPart() {
	int length = write_buffer.length();
	AmazonS3SendMultipartPart upload_part_request =
		AmazonS3SendMultipartPart(m_ai, m_object, m_log);
	if (!upload_part_request.SendRequest(
			write_buffer, std::to_string(partNumber), uploadId)) {
		m_log.Emsg("SendPart", "upload.SendRequest() failed");
		return -ENOENT;
	} else {
		m_log.Emsg("SendPart", "upload.SendRequest() succeeded");
		std::string resultString = upload_part_request.getResultString();
		std::size_t startPos = resultString.find("ETag:");
		std::size_t endPos = resultString.find("\"", startPos + 7);
		eTags.push_back(
			resultString.substr(startPos + 7, endPos - startPos - 7));

		partNumber++;
		write_buffer = "";
	}

	return length;
}

int S3File::Close(long long *retsz) {
	// this is only true if a buffer exists that needs to be drained
	if (write_buffer.length() > 0) {
		if (SendPart() == -ENOENT) {
			return -ENOENT;
		} else {
			m_log.Emsg("Close", "Closed our S3 file");
		}
	}
	// this is only true if some parts have been written and need to be
	// finalized
	if (partNumber > 1) {
		AmazonS3CompleteMultipartUpload complete_upload_request =
			AmazonS3CompleteMultipartUpload(m_ai, m_object, m_log);
		if (!complete_upload_request.SendRequest(eTags, partNumber, uploadId)) {
			m_log.Emsg("SendPart", "close.SendRequest() failed");
			return -ENOENT;
		} else {
			m_log.Emsg("SendPart", "close.SendRequest() succeeded");
		}
	}

	return 0;

	/* Original write code
	std::string payload((char *)buffer, size);
	if (!upload.SendRequest(payload, offset, size)) {
		m_log.Emsg("Open", "upload.SendRequest() failed");
		return -ENOENT;
	} else {
		m_log.Emsg("Open", "upload.SendRequest() succeeded");
		return 0;
	} */
}

extern "C" {

/*
	This function is called when we are wrapping something.
*/
XrdOss *XrdOssAddStorageSystem2(XrdOss *curr_oss, XrdSysLogger *Logger,
								const char *config_fn, const char *parms,
								XrdOucEnv *envP) {
	XrdSysError log(Logger, "s3_");

	log.Emsg("Initialize",
			 "S3 filesystem cannot be stacked with other filesystems");
	return nullptr;
}

/*
	This function is called when it is the top level file system and we are not
	wrapping anything
*/
XrdOss *XrdOssGetStorageSystem2(XrdOss *native_oss, XrdSysLogger *Logger,
								const char *config_fn, const char *parms,
								XrdOucEnv *envP) {
	auto log = new XrdSysError(Logger, "s3_");

	envP->Export("XRDXROOTD_NOPOSC", "1");

	try {
		AmazonRequest::Init(*log);
		g_s3_oss = new S3FileSystem(Logger, config_fn, envP);
		return g_s3_oss;
	} catch (std::runtime_error &re) {
		log->Emsg("Initialize", "Encountered a runtime failure", re.what());
		return nullptr;
	}
}

XrdOss *XrdOssGetStorageSystem(XrdOss *native_oss, XrdSysLogger *Logger,
							   const char *config_fn, const char *parms) {
	return XrdOssGetStorageSystem2(native_oss, Logger, config_fn, parms,
								   nullptr);
}

} // end extern "C"

XrdVERSIONINFO(XrdOssGetStorageSystem, s3);
XrdVERSIONINFO(XrdOssGetStorageSystem2, s3);
XrdVERSIONINFO(XrdOssAddStorageSystem2, s3);
