﻿/*
*    flv_media_stream_source.cpp:
*
*    Copyright (C) 2015-2017 Light Lin <blog.poxiao.me> All Rights Reserved.
*
*/

#include <algorithm>
#include <string>

#include "default_task_service.hpp"
#include "error.hpp"
#include "flv_media_stream_source.hpp"
#include "flv_player.hpp"

using namespace concurrency;
using namespace dawn_player;

using namespace Windows::Foundation::Collections;
using namespace Windows::Media::MediaProperties;

namespace dawn_player {

flv_media_stream_source::flv_media_stream_source()
{
}

void flv_media_stream_source::init(const std::shared_ptr<flv_player>& player, MediaStreamSource^ mss)
{
    this->player = player;
    this->mss = mss;
    this->tsk_service = this->player->get_task_service();
    starting_event_token = this->mss->Starting += ref new TypedEventHandler<MediaStreamSource^, MediaStreamSourceStartingEventArgs^>(this, &flv_media_stream_source::on_starting);
    sample_requested_event_token = this->mss->SampleRequested += ref new TypedEventHandler<MediaStreamSource^, MediaStreamSourceSampleRequestedEventArgs^>(this, &flv_media_stream_source::on_sample_requested);
}

IAsyncOperation<flv_media_stream_source^>^ flv_media_stream_source::create(IRandomAccessStream^ random_access_stream)
{
    return create(random_access_stream, true);
}

IAsyncOperation<flv_media_stream_source^>^ flv_media_stream_source::create(IRandomAccessStream^ random_access_stream, bool stream_can_seek)
{
    auto tce = task_completion_event<flv_media_stream_source^>();
    auto result_task = task<flv_media_stream_source^>(tce);
    auto tsk_service = std::make_shared<default_task_service>();
    tsk_service->post_task([tsk_service, random_access_stream, stream_can_seek, tce]() {
        auto player = std::make_shared<flv_player>(tsk_service);
        player->set_source(random_access_stream);
        player->open()
        .then([tsk_service, player, stream_can_seek, tce](task<std::map<std::string, std::string>> tsk) {
            tsk_service->post_task([player, stream_can_seek, tce, tsk]() {
                std::map<std::string, std::string> info;
                try {
                    info = tsk.get();
                }
                catch (const open_error&) {
                    tce.set_exception(ref new Platform::FailureException("Failed to open FLV file."));
                    return;
                }
                std::string acpd = info["AudioCodecPrivateData"];
                unsigned int channel_count = std::stol(acpd.substr(4, 2), 0, 16) + std::stol(acpd.substr(6, 2), 0, 16) * 0x100;
                unsigned int sample_rate = std::stol(acpd.substr(8, 2), 0, 16) + std::stol(acpd.substr(10, 2), 0, 16) * 0x100 +
                    std::stol(acpd.substr(12, 2), 0, 16) * 0x10000 + std::stol(acpd.substr(14, 2), 0, 16) * 0x1000000;
                unsigned int bit_rate = sample_rate * (std::stol(acpd.substr(28, 2), 0, 16) + std::stol(acpd.substr(30, 2), 0, 16) * 0x100);
                auto aep = AudioEncodingProperties::CreateAac(sample_rate, channel_count, bit_rate);
                auto asd = ref new AudioStreamDescriptor(aep);
                auto vep = VideoEncodingProperties::CreateH264();
                auto video_width = std::stoul(info["Width"]);
                auto video_height = std::stoul(info["Height"]);
                // It seems that H.264 only supports even numbered dimensions.
                vep->Width = video_width - (video_width % 2);
                vep->Height = video_height - (video_height % 2);
                auto vsd = ref new VideoStreamDescriptor(vep);
                auto mss = ref new MediaStreamSource(asd);
                mss->AddStreamDescriptor(vsd);
                mss->CanSeek = stream_can_seek && info["CanSeek"] == "True";
                // Set BufferTime to 0 to improve seek experience in Debug mode
                mss->BufferTime = TimeSpan{ 0 };
                auto iter_duration = info.find("Duration");
                if (iter_duration != info.end()) {
                    mss->Duration = TimeSpan{ std::stoll(std::get<1>(*iter_duration)) };
                }
                auto flv_mss = ref new flv_media_stream_source();
                flv_mss->init(player, mss);
                tce.set(flv_mss);
            });
        });
    });
    return create_async([result_task]() {
        return result_task;
    });
}

MediaStreamSource^ flv_media_stream_source::unwrap()
{
    return this->mss;
}

flv_media_stream_source::~flv_media_stream_source()
{
    if (this->player) {
        auto player = this->player;
        this->player = nullptr;
        this->tsk_service->post_task([player]() {
            create_async([player]() {
                return player->close();
            });
        });
    }
    if (this->mss) {
        this->mss->Starting -= this->starting_event_token;
        this->mss->SampleRequested -= this->sample_requested_event_token;
        this->mss = nullptr;
    }
}

void flv_media_stream_source::on_starting(MediaStreamSource^ sender, MediaStreamSourceStartingEventArgs^ args)
{
    auto request = args->Request;
    auto deferral = request->GetDeferral();
    this->tsk_service->post_task([this, request, deferral]() {
        auto start_position = request->StartPosition;
        if (start_position == nullptr) {
            request->SetActualStartPosition(TimeSpan{ this->player->get_start_position() });
            deferral->Complete();
        }
        else {
            create_async([this, start_position, request, deferral]() {
                return this->player->seek(start_position->Value.Duration)
                .then([this, request, deferral](task<std::int64_t> tsk) {
                    this->tsk_service->post_task([tsk, request, deferral]() {
                        auto seek_to_time = tsk.get();
                        request->SetActualStartPosition(TimeSpan{ seek_to_time });
                        deferral->Complete();
                    });
                });
            });
        }
    });
}

void flv_media_stream_source::on_sample_requested(MediaStreamSource^ sender, MediaStreamSourceSampleRequestedEventArgs^ args)
{
    auto request = args->Request;
    auto deferral = request->GetDeferral();
    this->tsk_service->post_task([this, sender, request, deferral]() {
        if (request->StreamDescriptor->GetType()->FullName == AudioStreamDescriptor::typeid->FullName) {
            create_async([this, sender, request, deferral]() {
                return this->player->get_audio_sample()
                    .then([this, sender, request, deferral](task<audio_sample> tsk) {
                    this->tsk_service->post_task([this, sender, request, deferral, tsk]() {
                        try {
                            audio_sample sample = tsk.get();
                            auto data_writer = ref new DataWriter();
                            for (auto byte : sample.data) {
                                data_writer->WriteByte(byte);
                            }
                            auto stream_sample = MediaStreamSample::CreateFromBuffer(data_writer->DetachBuffer(), TimeSpan{ sample.timestamp });
                            request->Sample = stream_sample;
                        }
                        catch (const get_sample_error& gse) {
                            if (gse.code() == get_sample_error_code::end_of_stream) {
                                request->Sample = nullptr;
                            }
                            else {
                                sender->NotifyError(MediaStreamSourceErrorStatus::Other);
                            }
                        }
                        deferral->Complete();
                    });
                });
            }); 
        }
        else if (request->StreamDescriptor->GetType()->FullName == VideoStreamDescriptor::typeid->FullName) {
            create_async([this, sender, request, deferral]() {
                return this->player->get_video_sample()
                .then([this, sender, request, deferral](task<video_sample> tsk) {
                    this->tsk_service->post_task([this, sender, request, deferral, tsk]() {
                        try {
                            video_sample sample = tsk.get();
                            auto data_writer = ref new DataWriter();
                            if (sample.is_key_frame) {
                                const auto& sps = this->player->get_sps();
                                if (!sps.empty()) {
                                    data_writer->WriteByte(0);
                                    data_writer->WriteByte(0);
                                    data_writer->WriteByte(1);
                                    for (auto byte : sps) {
                                        data_writer->WriteByte(byte);
                                    }
                                }
                                const auto& pps = this->player->get_pps();
                                if (!pps.empty()) {
                                    data_writer->WriteByte(0);
                                    data_writer->WriteByte(0);
                                    data_writer->WriteByte(1);
                                    for (auto byte : pps) {
                                        data_writer->WriteByte(byte);
                                    }
                                }
                            }
                            for (auto byte : sample.data) {
                                data_writer->WriteByte(byte);
                            }
                            auto stream_sample = MediaStreamSample::CreateFromBuffer(data_writer->DetachBuffer(), TimeSpan{ sample.timestamp });
                            stream_sample->DecodeTimestamp = TimeSpan{ sample.dts };
                            stream_sample->KeyFrame = sample.is_key_frame;
                            request->Sample = stream_sample;
                        }
                        catch (const get_sample_error& gse) {
                            if (gse.code() == get_sample_error_code::end_of_stream) {
                                request->Sample = nullptr;
                            }
                            else {
                                sender->NotifyError(MediaStreamSourceErrorStatus::Other);
                            }
                        }
                        deferral->Complete();
                    });
                });
            });
        }
    });
}

} // namespace dawn_player
