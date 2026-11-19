#include "mac_preview.hh"

#include <QWidget>

#import <AppKit/AppKit.h>
#import <AVFoundation/AVFoundation.h>

#include <atomic>

@interface VtbCameraFrames : NSObject <AVCaptureVideoDataOutputSampleBufferDelegate> {
@public
    std::atomic<quint64> frames;
}
@property(nonatomic, copy) NSString* failure;
@end

@implementation VtbCameraFrames
- (void)captureOutput:(AVCaptureOutput*)output
    didOutputSampleBuffer:(CMSampleBufferRef)sample
           fromConnection:(AVCaptureConnection*)connection {
    (void)output;
    (void)connection;
    if (CMSampleBufferGetImageBuffer(sample)) {
        frames.fetch_add(1);
    }
}
@end

namespace vtb::desktop {
namespace {

    auto external_cameras() -> NSArray<AVCaptureDevice*>* {
        AVCaptureDeviceType type;
        if (@available(macOS 14.0, *)) {
            type = AVCaptureDeviceTypeExternal;
        } else {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
            type = AVCaptureDeviceTypeExternalUnknown;
#pragma clang diagnostic pop
        }
        return [AVCaptureDeviceDiscoverySession discoverySessionWithDeviceTypes:@[ type ]
            mediaType:AVMediaTypeVideo position:AVCaptureDevicePositionUnspecified].devices;
    }

} // namespace

class MacPreview::Impl final {
public:
    explicit Impl(QWidget& host) : host_(host) { }
    ~Impl() = default;
    Impl(const Impl&) = delete;
    auto operator=(const Impl&) -> Impl& = delete;
    Impl(Impl&&) = delete;
    auto operator=(Impl&&) -> Impl& = delete;

    auto view() -> NSView* {
        auto* view = (__bridge NSView*)reinterpret_cast<void*>(host_.winId());
        view.wantsLayer = YES;
        return view;
    }

    QWidget& host_;
    dispatch_queue_t camera_queue_ = dispatch_queue_create("org.vtb.desktop.capture", DISPATCH_QUEUE_SERIAL);
    dispatch_queue_t frame_queue_ = dispatch_queue_create("org.vtb.desktop.frames", DISPATCH_QUEUE_SERIAL);
    AVCaptureSession* session_;
    VtbCameraFrames* camera_state_;
    AVCaptureVideoPreviewLayer* camera_layer_;
};

MacPreview::MacPreview(QWidget& host) : impl_(std::make_unique<Impl>(host)) { }

MacPreview::~MacPreview() {
    stop();
    dispatch_sync(impl_->camera_queue_, ^{});
}

auto MacPreview::cameras() -> QVector<CameraChoice> {
    QVector<CameraChoice> result;
    for (AVCaptureDevice* camera in external_cameras()) {
        result.push_back({ QString::fromNSString(camera.uniqueID),
            QString::fromNSString(camera.localizedName) });
    }
    return result;
}

auto MacPreview::cameraPermission() -> CameraPermission {
    const auto status = [AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeVideo];
    if (status == AVAuthorizationStatusAuthorized) {
        return CameraPermission::ALLOWED;
    }
    if (status == AVAuthorizationStatusNotDetermined) {
        return CameraPermission::UNDETERMINED;
    }
    return CameraPermission::DENIED;
}

auto MacPreview::requestCameraPermission(std::function<void(bool)> reply) -> void {
    [AVCaptureDevice requestAccessForMediaType:AVMediaTypeVideo completionHandler:^(BOOL allowed) {
        dispatch_async(dispatch_get_main_queue(), ^{ reply(allowed); });
    }];
}

auto MacPreview::startCamera(const QString& id) -> void {
    stop();
    AVCaptureDevice* device = nil;
    NSString* identifier = id.toNSString();
    for (AVCaptureDevice* candidate in external_cameras()) {
        if ([candidate.uniqueID isEqualToString:identifier]) {
            device = candidate;
            break;
        }
    }
    auto* state = [[VtbCameraFrames alloc] init];
    impl_->camera_state_ = state;
    if (!device) {
        state.failure = @"在 Mac 上找不到此 UVC 设备。";
        return;
    }
    auto* session = [[AVCaptureSession alloc] init];
    impl_->session_ = session;
    auto* layer = [AVCaptureVideoPreviewLayer layerWithSession:session];
    layer.videoGravity = AVLayerVideoGravityResizeAspect;
    impl_->camera_layer_ = layer;
    auto* view = impl_->view();
    layer.frame = view.bounds;
    layer.autoresizingMask = kCALayerWidthSizable | kCALayerHeightSizable;
    [view.layer addSublayer:layer];
    const auto frame_queue = impl_->frame_queue_;
    dispatch_async(impl_->camera_queue_, ^{
        NSError* failure = nil;
        auto* input = [AVCaptureDeviceInput deviceInputWithDevice:device error:&failure];
        auto* output = [[AVCaptureVideoDataOutput alloc] init];
        output.alwaysDiscardsLateVideoFrames = YES;
        if (!input || ![session canAddInput:input]) {
            NSString* message = failure.localizedDescription;
            if (!message) {
                message = @"无法打开选中的摄像头。";
            }
            dispatch_async(dispatch_get_main_queue(), ^{ state.failure = message; });
            return;
        }
        [session beginConfiguration];
        [session addInput:input];
        const BOOL can_monitor = [session canAddOutput:output];
        if (can_monitor) {
            [output setSampleBufferDelegate:state queue:frame_queue];
            [session addOutput:output];
        }
        [session commitConfiguration];
        if (can_monitor) {
            [session startRunning];
        } else {
            dispatch_async(dispatch_get_main_queue(), ^{ state.failure = @"无法读取摄像头视频帧。"; });
        }
    });
}

auto MacPreview::stop() -> void {
    auto* session = impl_->session_;
    if (session) {
        dispatch_async(impl_->camera_queue_, ^{ [session stopRunning]; });
    }
    [impl_->camera_layer_ removeFromSuperlayer];
    impl_->camera_layer_ = nil;
    impl_->session_ = nil;
    impl_->camera_state_ = nil;
}

auto MacPreview::frames() const -> quint64 {
    if (impl_->camera_state_) {
        return impl_->camera_state_->frames.load();
    }
    return 0;
}

auto MacPreview::error() const -> QString {
    if (impl_->camera_state_) {
        return QString::fromNSString(impl_->camera_state_.failure);
    }
    return {};
}

} // namespace vtb::desktop
