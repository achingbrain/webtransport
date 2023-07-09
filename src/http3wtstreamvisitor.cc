// Copyright (c) 2022 Marten Richter or other contributers (see commit). All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/http3wtstreamvisitor.h"
#include "src/http3server.h"

namespace quic
{
    void Http3WTStreamJS::init(Http3WTStream * wtstream)
    {
        wtstream_ = std::unique_ptr<Http3WTStream>(wtstream);
    }

    Http3WTStream::Visitor::~Visitor()
    {
        while (stream_->chunks_.size() > 0)
        {
            auto cur = stream_->chunks_.front();

            // now we have to inform the server TODO
            stream_->eventloop_->informAboutStreamWrite(stream_, cur.bufferhandle, false);

            stream_->chunks_.pop_front();
        }

        stream_->eventloop_->informAboutStreamRead(stream_, 0, false, false);

        if (!stream_->stop_sending_received_)
        {
            stream_->eventloop_->informStreamRecvSignal(stream_, 0, NetworkTask::stopSending);
        }
        if (!stream_->stream_was_reset_)
        {
            stream_->eventloop_->informStreamRecvSignal(stream_, 0, NetworkTask::resetStream);
        }
        Http3WTStreamJS *strobj = stream_->getJS();
        if (strobj) {
            stream_->stream_ = nullptr;
            stream_->eventloop_->informUnref(strobj);
        } else {
            stream_->stream_ = nullptr;
        }
    }

    void Http3WTStream::Visitor::OnWriteSideInDataRecvdState() // called if everything is written to the client and it is closed
    {
        if (stream_->send_fin_)
            stream_->eventloop_->informAboutStreamNetworkFinish(stream_, NetworkTask::streamFinal);
    }

    void Http3WTStream::Visitor::OnResetStreamReceived(WebTransportStreamError error)
    {
        // should this be removed
        /*

        // Send FIN in response to a stream reset.  We want to test that we can
        // operate one side of the stream cleanly while the other is reset, thus
        // replying with a FIN rather than a RESET_STREAM is more appropriate here.
        stream_->send_fin_ = true;
        OnCanWrite();*/
        stream_->stream_was_reset_ = true;
        lasterror = error;
        stream_->eventloop_->informStreamRecvSignal(stream_, error, NetworkTask::resetStream); // may be move below
    }

    void Http3WTStream::Visitor::OnStopSendingReceived(WebTransportStreamError error)
    {
        stream_->stop_sending_received_ = true;
        stream_->eventloop_->informStreamRecvSignal(stream_, error, NetworkTask::stopSending); // may be move below
        // we should also finallize the stream, so send a fin
        stream_->send_fin_ = true;
        OnCanWrite();
    }

    void Http3WTStream::cancelWrite(Napi::ObjectReference *handle)
    {
        eventloop_->informAboutStreamWrite(this, handle, false);
    }

    void Http3WTStream::doCanRead()
    {
        // if (pause_reading_) return ; // back pressure folks!
        
        if (pause_reading_)
        {
            can_read_pending_ = true;
            return; // back pressure folks!
        }
        // first figure out if we have readable data
        if (bufferlen_ >= readbufsize_ || !readbufdata_) {
            can_read_pending_ = true;
            return; // no space here
        }
        size_t readable = stream_->ReadableBytes();
        bool read = false;
        while (readable > 0 && bufferlen_ < readbufsize_)
        {
            if (writepos_ >= readpos_) {
                size_t len = readbufsize_ - writepos_;   
                WebTransportStream::ReadResult result = 
                    stream_->Read(absl::Span<char>(((char*) readbufdata_) + writepos_, len));
                QUIC_DVLOG(1) << "Attempted reading on WebTransport stream "
                          << ", bytes read: " << result.bytes_read;
                writepos_ = (writepos_ + result.bytes_read) % readbufsize_;
                bufferlen_ = bufferlen_ + result.bytes_read;
                eventloop_->informAboutStreamRead(this, result.bytes_read, 
                            result.fin, true);
            } else  { // readpos_ > writepos_
                size_t len = writepos_ - readpos_;
                WebTransportStream::ReadResult result = 
                    stream_->Read(absl::Span<char>(((char*) readbufdata_) + writepos_, len));
                QUIC_DVLOG(1) << "Attempted reading on WebTransport stream "
                          << ", bytes read: " << result.bytes_read; 
                writepos_ = (writepos_ + result.bytes_read) % readbufsize_;
                bufferlen_ = bufferlen_ + result.bytes_read;
                eventloop_->informAboutStreamRead(this, result.bytes_read,
                             result.fin, true);
            } 
            readable = stream_->ReadableBytes();
        }
    }

    void Http3WTStream::doCanWrite()
    {
        /* if (/* stop_sending_received_ || * pause_reading_)
         {
             return;
         } */
        if (fin_was_sent_) return;

        while (chunks_.size() > 0)
        {
            auto cur = chunks_.front();
            bool success = stream_->Write(absl::string_view(cur.buffer, cur.len));
            QUIC_DVLOG(1) << "Attempted writing on WebTransport bidirectional stream "
                          << ", success: " << (success ? "yes" : "no");
            if (!success)
            {
                return;
            }
            // now we have to inform the server TODO
            eventloop_->informAboutStreamWrite(this, cur.bufferhandle, true);

            chunks_.pop_front();
        }

        if (send_fin_)
        {
            bool success = stream_->SendFin();
            if (success) fin_was_sent_ = true;
        }
    }

}
