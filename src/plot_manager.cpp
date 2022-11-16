
#include <cstdlib>
#include <string>
#include <vector>

#ifdef __APPLE__
    #include <OpenGL/gl.h>
#endif

#include "htslib/faidx.h"
#include "htslib/hfile.h"
#include "htslib/hts.h"
#include "htslib/sam.h"

#include <GLFW/glfw3.h>
#define SK_GL
#include "include/gpu/GrBackendSurface.h"
#include "include/gpu/GrDirectContext.h"
#include "include/gpu/gl/GrGLInterface.h"
#include "include/core/SkCanvas.h"
#include "include/core/SkColorSpace.h"
#include "include/core/SkSamplingOptions.h"
#include "include/core/SkSurface.h"

#include "drawing.h"
#include "plot_manager.h"
#include "segments.h"
#include "../include/termcolor.h"
#include "themes.h"


using namespace std::literals;

namespace Manager {

    void HiddenWindow::init(int width, int height) {
        if (!glfwInit()) {
            std::cerr<<"ERROR: could not initialize GLFW3"<<std::endl;
            std::terminate();
        }
        glfwWindowHint(GLFW_STENCIL_BITS, 8);
        glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
        window = glfwCreateWindow(width, height, "GW", NULL, NULL);
        if (!window) {
            std::cerr<<"ERROR: could not create back window with GLFW3"<<std::endl;
            glfwTerminate();
            std::terminate();
        }
        glfwMakeContextCurrent(window);
    }

    GwPlot::GwPlot(std::string reference, std::vector<std::string> &bampaths, Themes::IniOptions &opt, std::vector<Utils::Region> &regions,
                   std::vector<std::string> &track_paths) {
        this->reference = reference;
        this->bam_paths = bampaths;
        this->regions = regions;
        this->opts = opt;
        redraw = true;
        processed = false;
        calcScaling = true;
        drawToBackWindow = false;
        fonts = Themes::Fonts();
        fai = fai_load(reference.c_str());
        for (auto &fn: bampaths) {
            htsFile* f = sam_open(fn.c_str(), "r");
            hts_set_fai_filename(f, reference.c_str());
            hts_set_threads(f, opt.threads);
            bams.push_back(f);
            sam_hdr_t *hdr_ptr = sam_hdr_read(f);
            headers.push_back(hdr_ptr);
            hts_idx_t* idx = sam_index_load(f, fn.c_str());
            indexes.push_back(idx);
        }
        tracks.resize(track_paths.size());
        int i = 0;
        for (auto &tp: track_paths) {
            tracks[i].open(tp, true);
            i += 1;
        }
        linked.resize(bams.size());
        samMaxY = 0;
        yScaling = 0;
        captureText = shiftPress = ctrlPress = processText = false;
        xDrag = xOri = -1000000;
        lastX = -1;
        commandIndex = 0;
        blockStart = 0;
        regionSelection = 0;
        mode = Show::SINGLE;
    }

    GwPlot::~GwPlot() {
        if (window) {
            glfwDestroyWindow(window);
        }
        if (backWindow) {
            glfwDestroyWindow(backWindow);
        }
        glfwTerminate();
//        if (clicked.refSeq != nullptr) {
//            delete clicked.refSeq;
//        }
//        for (auto &rgn : regions) {
//            if (rgn.refSeq != nullptr) {
//                delete rgn.refSeq;
//            }
//        }
        for (auto &bm : bams) {
            hts_close(bm);
        }
        for (auto &hd: headers) {
            bam_hdr_destroy(hd);
        }
        for (auto &idx: indexes) {
            hts_idx_destroy(idx);
        }
        fai_destroy(fai);
    }

    void GwPlot::init(int width, int height) {

        if (!glfwInit()) {
            std::cerr<<"ERROR: could not initialize GLFW3"<<std::endl;
            std::terminate();
        }

        glfwWindowHint(GLFW_STENCIL_BITS, 8);

        window = glfwCreateWindow(width, height, "GW", NULL, NULL);

        // https://stackoverflow.com/questions/7676971/pointing-to-a-function-that-is-a-class-member-glfw-setkeycallback/28660673#28660673
        glfwSetWindowUserPointer(window, this);

        auto func_key = [](GLFWwindow* w, int k, int s, int a, int m){
            static_cast<GwPlot*>(glfwGetWindowUserPointer(w))->keyPress(w, k, s, a, m);
        };
        glfwSetKeyCallback(window, func_key);

        auto func_drop = [](GLFWwindow* w, int c, const char**paths){
            static_cast<GwPlot*>(glfwGetWindowUserPointer(w))->pathDrop(w, c, paths);
        };
        glfwSetDropCallback(window, func_drop);

        auto func_mouse = [](GLFWwindow* w, int b, int a, int m){
            static_cast<GwPlot*>(glfwGetWindowUserPointer(w))->mouseButton(w, b, a, m);
        };
        glfwSetMouseButtonCallback(window, func_mouse);

        auto func_pos = [](GLFWwindow* w, double x, double y){
            static_cast<GwPlot*>(glfwGetWindowUserPointer(w))->mousePos(w, x, y);
        };
        glfwSetCursorPosCallback(window, func_pos);

        auto func_scroll = [](GLFWwindow* w, double x, double y){
            static_cast<GwPlot*>(glfwGetWindowUserPointer(w))->scrollGesture(w, x, y);
        };
        glfwSetScrollCallback(window, func_scroll);

        auto func_resize = [](GLFWwindow* w, int x, int y){
            static_cast<GwPlot*>(glfwGetWindowUserPointer(w))->windowResize(w, x, y);
        };
        glfwSetWindowSizeCallback(window, func_resize);

        if (!window) {
            std::cerr<<"ERROR: could not create window with GLFW3"<<std::endl;
            glfwTerminate();
            std::terminate();
        }
        glfwMakeContextCurrent(window);

    }

    void GwPlot::initBack(int width, int height) {
        if (!glfwInit()) {
            std::cerr<<"ERROR: could not initialize GLFW3"<<std::endl;
            std::terminate();
        }
        glfwWindowHint(GLFW_STENCIL_BITS, 8);
        glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
        backWindow = glfwCreateWindow(width, height, "GW", NULL, NULL);
        if (!backWindow) {
            std::cerr<<"ERROR: could not create back window with GLFW3"<<std::endl;
            glfwTerminate();
            std::terminate();
        }
        glfwMakeContextCurrent(backWindow);
        drawToBackWindow = true;
    }

    void GwPlot::fetchRefSeq(Utils::Region &rgn) {
        int rlen = rgn.end - rgn.start - 1;
        rgn.refSeq = faidx_fetch_seq(fai, rgn.chrom.c_str(), rgn.start, rgn.end - 1, &rlen);
    }

    void GwPlot::fetchRefSeqs() {
        for (auto &rgn : regions) {
            fetchRefSeq(rgn);
        }
    }

    void GwPlot::setVariantFile(std::string &path, int startIndex, bool cacheStdin) {

        if (cacheStdin || Utils::endsWith(path, ".vcf") ||
            Utils::endsWith(path, ".vcf.gz") ||
            Utils::endsWith(path, ".bcf")) {
            variantTrack.done = true;
            mouseOverTileIndex = -1;
            vcf.seenLabels = &seenLabels;
            vcf.cacheStdin = cacheStdin;
            vcf.label_to_parse = opts.parse_label.c_str();
            vcf.open(path);  // todo some error checking needed?
            if (startIndex > 0) {
                int bLen = opts.number.x * opts.number.y;
                bool done = false;
                while (!done) {
                    if (vcf.done) {
                        done = true;
                    } else {
                        for (int i=0; i < bLen; ++ i) {
                            if (vcf.done) {
                                done = true;
                                break;
                            }
                            vcf.next();
                            appendVariantSite(vcf.chrom, vcf.start, vcf.chrom2, vcf.stop, vcf.rid, vcf.label, vcf.vartype);
                        }
                        if (blockStart + bLen > startIndex) {
                            done = true;
                        } else if (!done) {
                            blockStart += bLen;
                        }
                    }
                }
            }
        } else {  // bed file or labels file, or some other tsv
            vcf.done = true;
            variantTrack.open(path, false);
            variantTrack.fetch(nullptr);  // initialize iterators
            if (startIndex > 0) {
                int bLen = opts.number.x * opts.number.y;
                bool done = false;
                while (!done) {
                    if (variantTrack.done) {
                        done = true;
                    } else {
                        for (int i=0; i < bLen; ++ i) {
                            if (variantTrack.done) {
                                done = true;
                                break;
                            }
                            variantTrack.next();
                            std::string label = "";
                            appendVariantSite(variantTrack.chrom, variantTrack.start, variantTrack.chrom2,
                                              variantTrack.stop, variantTrack.rid, label, variantTrack.vartype);
                        }
                        if (blockStart + bLen > startIndex) {
                            done = true;
                        } else if (!done) {
                            blockStart += bLen;
                        }
                    }
                }
            }
        }
    }

    void GwPlot::setOutLabelFile(const std::string &path) {
        outLabelFile = path;
    }

    void GwPlot::saveLabels() {
        if (!outLabelFile.empty()) Utils::saveLabels(multiLabels, outLabelFile);
    }

    void GwPlot::setLabelChoices(std::vector<std::string> &labels) {
        labelChoices = labels;
    }

    void GwPlot::setVariantSite(std::string &chrom, long start, std::string &chrom2, long stop) {
        this->clearCollections();
        long rlen = stop - start;
        bool isTrans = chrom != chrom2;
        if (!isTrans && rlen <= opts.split_view_size) {
            regions.resize(1);
            regions[0].chrom = chrom;
            regions[0].start = (1 > start - opts.pad) ? 1 : start - opts.pad;
            regions[0].end = stop + opts.pad;
            regions[0].markerPos = start;
            regions[0].markerPosEnd = stop;
        } else {
            regions.resize(2);
            regions[0].chrom = chrom;
            regions[0].start = (1 > start - opts.pad) ? 1 : start - opts.pad;
            regions[0].end = start + opts.pad;
            regions[0].markerPos = start;
            regions[0].markerPosEnd = (isTrans) ? start : stop;
            regions[1].chrom = chrom2;
            regions[1].start = (1 > stop - opts.pad) ? 1 : stop - opts.pad;
            regions[1].end = stop + opts.pad;
            regions[1].markerPos = stop;
            regions[1].markerPosEnd = (isTrans) ? stop : start;
        }
    }

    void GwPlot::appendVariantSite(std::string &chrom, long start, std::string &chrom2, long stop, std::string &rid, std::string &label, std::string &vartype) {
        this->clearCollections();
        long rlen = stop - start;
        std::vector<Utils::Region> v;
        bool isTrans = chrom != chrom2;
        if (!isTrans && rlen <= opts.split_view_size) {
            Utils::Region r;
            v.resize(1);
            v[0].chrom = chrom;
            v[0].start = (1 > start - opts.pad) ? 1 : start - opts.pad;
            v[0].end = stop + opts.pad;
            v[0].markerPos = start;
            v[0].markerPosEnd = stop;
        } else {
            v.resize(2);
            v[0].chrom = chrom;
            v[0].start = (1 > start - opts.pad) ? 1 : start - opts.pad;
            v[0].end = start + opts.pad;
            v[0].markerPos = start;
            v[0].markerPosEnd = (isTrans) ? start : stop;
            v[1].chrom = chrom2;
            v[1].start = (1 > stop - opts.pad) ? 1 : stop - opts.pad;
            v[1].end = stop + opts.pad;
            v[1].markerPos = stop;
            v[1].markerPosEnd = (isTrans) ? stop : start;

        }
        multiRegions.push_back(v);
        if (inputLabels.contains(rid)) {
            multiLabels.push_back(inputLabels[rid]);
        } else {
            multiLabels.push_back(Utils::makeLabel(chrom, start, label, labelChoices, rid, vartype, "", 0));
        }
    }

    int GwPlot::startUI(GrDirectContext* sContext, SkSurface *sSurface) {

        std::cout << "Type ':help' or ':h' for more info\n";

        setGlfwFrameBufferSize();
        fetchRefSeqs();
        opts.theme.setAlphas();
        GLFWwindow * wind = this->window;
        if (mode == Show::SINGLE) {
            printRegionInfo();
        } else {
            bboxes = Utils::imageBoundingBoxes(opts.number, (float)fb_width, (float)fb_height);
            std::cout << termcolor::green << "Index     " << termcolor::reset << blockStart << std::endl;
        }
        bool wasResized = false;
        std::chrono::high_resolution_clock::time_point autoSaveTimer = std::chrono::high_resolution_clock::now();

        while (true) {
            if (glfwWindowShouldClose(wind)) {
                break;
            } else if (glfwGetKey(wind, GLFW_KEY_ESCAPE) == GLFW_PRESS) {
                break;
            }
            glfwWaitEvents();
            if (redraw) {
                if (mode == Show::SINGLE) {
                    drawScreen(sSurface->getCanvas(), sContext);
                } else {
                    drawTiles(sSurface->getCanvas(), sContext, sSurface);
                }
            }
            if (resizeTriggered && std::chrono::duration_cast<std::chrono::milliseconds >(std::chrono::high_resolution_clock::now() - resizeTimer) > 100ms) {
                imageCache.clear();
                redraw = true;
                processed = false;
                wasResized = true;

                glfwGetFramebufferSize(window, &fb_width, &fb_height);
                bboxes = Utils::imageBoundingBoxes(opts.number, (float)fb_width, (float)fb_height);

                opts.dimensions.x = fb_width;
                opts.dimensions.y = fb_height;
                resizeTriggered = false;

                GrGLFramebufferInfo framebufferInfo;
                framebufferInfo.fFBOID = 0;
                framebufferInfo.fFormat = GL_RGBA8;
                GrBackendRenderTarget backendRenderTarget(fb_width, fb_height, 0, 0, framebufferInfo);

                if (!backendRenderTarget.isValid()) {
                    std::cerr << "ERROR: backendRenderTarget was invalid" << std::endl;
                    glfwTerminate();
                    std::terminate();
                }

                sSurface = SkSurface::MakeFromBackendRenderTarget(sContext,
                                                                  backendRenderTarget,
                                                                  kBottomLeft_GrSurfaceOrigin,
                                                                  kRGBA_8888_SkColorType,
                                                                  nullptr,
                                                                  nullptr).release();
                if (!sSurface) {
                    std::cerr << "ERROR: sSurface could not be initialized (nullptr). The frame buffer format needs changing\n";
                    std::terminate();
                }
                resizeTimer = std::chrono::high_resolution_clock::now();

            }
            if (std::chrono::duration_cast<std::chrono::milliseconds >(std::chrono::high_resolution_clock::now() - autoSaveTimer) > 1min) {
                saveLabels();
                autoSaveTimer = std::chrono::high_resolution_clock::now();
            }
        }
        saveLabels();
        if (wasResized) {
            // no idea why, but unless exit is here then we get an abort error if we return to main. Something to do with lifetime of backendRenderTarget
            std::cout << "\nGw finished\n";
            exit(EXIT_SUCCESS);
        }

        return 1;
    }

    void GwPlot::clearCollections() {
        regions.clear();
        for (auto & cl : collections) {
            for (auto & a : cl.readQueue) {
                bam_destroy1(a.delegate);
            }
//            if (cl.region.refSeq != nullptr) {
//                delete cl.region.refSeq;
//            }
        }
        collections.clear();
    }

    void GwPlot::processBam() {  // collect reads, calc coverage and find y positions on plot
        if (!processed) {
            if (opts.link_op != 0) {
                linked.clear();
                linked.resize(bams.size() * regions.size());
            }
            int idx = 0;
            if (collections.size() != bams.size() * regions.size()) {
                collections.clear();
                collections.resize(bams.size() * regions.size());
            } else {
                for (auto &cl: collections) {
                    cl.readQueue.clear();
                    cl.covArr.clear();
                    cl.levelsStart.clear();
                    cl.levelsEnd.clear();
                }
            }

            for (int i=0; i<(int)bams.size(); ++i) {
                htsFile* b = bams[i];
                sam_hdr_t *hdr_ptr = headers[i];
                hts_idx_t *index = indexes[i];

                for (int j=0; j<(int)regions.size(); ++j) {
                    Utils::Region *reg = &regions[j];
                    collections[idx].bamIdx = i;
                    collections[idx].regionIdx = j;
                    collections[idx].region = regions[j];
                    if (opts.coverage) {
                        collections[idx].covArr.resize(reg->end - reg->start + 1, 0);
                    }
                    HGW::collectReadsAndCoverage(collections[idx], b, hdr_ptr, index,opts, reg, opts.coverage, filters);

                    int maxY = Segs::findY(idx, collections[idx], collections[idx].readQueue, opts.link_op, opts, reg, linked, false);
                    if (maxY > samMaxY) {
                        samMaxY = maxY;
                    }
                    idx += 1;
                }
            }

            if (bams.empty()) {
                collections.resize(regions.size());
                for (int j=0; j<(int)regions.size(); ++j) {
                    collections[idx].bamIdx = -1;
                    collections[idx].regionIdx = j;
                    collections[idx].region = regions[j];
                    idx += 1;
                }
            }

        }
    }

    void GwPlot::setGlfwFrameBufferSize() {
        if (!drawToBackWindow) {
            glfwGetFramebufferSize(window, &fb_width, &fb_height);
        } else {
            glfwGetFramebufferSize(backWindow, &fb_width, &fb_height);
        }
    }

    void GwPlot::setScaling() {  // sets z_scaling, y_scaling trackY and regionWidth
//        if (samMaxY == 0 || !calcScaling) {
//            return;
//        }
        float xscale, yscale;
        if (drawToBackWindow) {
            glfwGetWindowContentScale(backWindow, &xscale, &yscale);
        } else {
            glfwGetWindowContentScale(window, &xscale, &yscale);
        }

        refSpace = fb_height * 0.02;
        auto fbh = (float) fb_height; // - refSpace;
        auto fbw = (float) fb_width;
        if (bams.empty()) {
            covY = 0; totalCovY = 0; totalTabixY = 0; tabixY = 0;
//            return;
        }
        auto nbams = (float)bams.size();
        if (opts.coverage && nbams > 0) {
            totalCovY = fbh * 0.1;
            covY = totalCovY / nbams;
        } else {
            totalCovY = 0; covY = 0;
        }
        float gap = 6; //fbw * 0.002;
        float gap2 = gap*2;

        if (tracks.empty()) {
            totalTabixY = 0; tabixY = 0;
        } else {
            totalTabixY = fbh * (0.05 * tracks.size());
            if (totalTabixY > 0.15 * fbh) {
                totalTabixY = 0.15 * fbh;
            }
            tabixY = totalTabixY / tracks.size();
        }
        if (nbams) {
            trackY = (fbh - totalCovY - totalTabixY - gap2 - refSpace) / nbams;
            yScaling = ((fbh - totalCovY - totalTabixY - gap2 - refSpace) / (float)samMaxY) / nbams;
        } else {
            trackY = 0;
            yScaling = 0;
        }
        fonts.setFontSize(yScaling, yscale);
        regionWidth = fbw / (float)regions.size();
        bamHeight = covY + trackY; // + tabixY;

        for (auto &cl: collections) {
            cl.xScaling = (regionWidth - gap2) / ((double)(cl.region.end - cl.region.start));
            cl.xOffset = (regionWidth * (float)cl.regionIdx) + gap;
            cl.yOffset = (float)cl.bamIdx * bamHeight + covY + refSpace;
            cl.yPixels = trackY + covY; // + tabixY;
        }
    }

    void GwPlot::drawScreen(SkCanvas* canvas, GrDirectContext* sContext) {
        canvas->drawPaint(opts.theme.bgPaint);
        if (!regions.empty()) {
            processBam();
            setGlfwFrameBufferSize();
            setScaling();
            if (opts.coverage) {
                Drawing::drawCoverage(opts, collections, canvas, fonts, covY, refSpace);
            }
            Drawing::drawBams(opts, collections, canvas, yScaling, fonts, linked, opts.link_op, refSpace);
            Drawing::drawRef(opts, regions, fb_width, canvas, fonts, refSpace, (float)regions.size());
            Drawing::drawBorders(opts, fb_width, fb_height, canvas, regions.size(), bams.size(), totalTabixY, tabixY, tracks.size());
            Drawing::drawTracks(opts, fb_width, fb_height, canvas, totalTabixY, tabixY, tracks, regions, fonts);

        }
        sContext->flush();
        glfwSwapBuffers(window);
        redraw = false;
    }

    void GwPlot::tileDrawingThread(SkCanvas* canvas, GrDirectContext* sContext, SkSurface *sSurface) {
        int bStart = blockStart;
        int bLen = (int)opts.number.x * (int)opts.number.y;
        int endIdx = bStart + bLen;
        for (int i=bStart; i<endIdx; ++i) {
            bool c = imageCache.contains(i);
            if (!c && i < (int)multiRegions.size() && !bams.empty()) {
                regions = multiRegions[i];
                runDraw(canvas);
                sk_sp<SkImage> img(sSurface->makeImageSnapshot());
                imageCache[i] = img;
                sContext->flush();
//                glfwPostEmptyEvent();
//                mtx.unlock();
            }
        }
    }

    void GwPlot::drawTiles(SkCanvas* canvas, GrDirectContext* sContext, SkSurface *sSurface) {
        int bStart = blockStart;
        int bLen = opts.number.x * opts.number.y;

        if (!vcf.done && bStart + bLen > (int)multiRegions.size()) {
            for (int i=0; i < bLen; ++ i) {
                vcf.next();
                if (vcf.done) {
                    break;
                }
                appendVariantSite(vcf.chrom, vcf.start, vcf.chrom2, vcf.stop, vcf.rid, vcf.label, vcf.vartype);
            }
        } else if (!variantTrack.done) {
            std::string empty_label = "";
            for (int i=0; i < bLen; ++ i) {
                variantTrack.next();
                if (variantTrack.done) {
                    break;
                }
                appendVariantSite(variantTrack.chrom, variantTrack.start, variantTrack.chrom, variantTrack.stop, variantTrack.rid, empty_label, variantTrack.vartype);
            }
        }
        setGlfwFrameBufferSize();
        setScaling();
        bboxes = Utils::imageBoundingBoxes(opts.number, fb_width, fb_height);
        SkSamplingOptions sampOpts = SkSamplingOptions();

        std::vector<sk_sp<SkImage>> blockImages;

        tileDrawingThread(canvas, sContext, sSurface);

        int i = bStart;
        canvas->drawPaint(opts.theme.bgPaint);

        std::vector<std::string> srtLabels;
        for (auto &itm: seenLabels) {
            srtLabels.push_back(itm);
        }
        std::sort(srtLabels.begin(), srtLabels.end());
        auto pivot = std::find_if(srtLabels.begin(),  // put PASS at front if available, forces labels to have some order
                                  srtLabels.end(),
                                  [](const std::string& s) -> bool {
                                      return s == "PASS";
                                  });
        if (pivot != srtLabels.end()) {
            std::rotate(srtLabels.begin(), pivot, pivot + 1);
        }

        for (auto &b : bboxes) {
            SkRect rect;
            if (imageCache.contains(i)) {
                rect.setXYWH(b.xStart, b.yStart, b.width, b.height);
                canvas->drawImageRect(imageCache[i], rect, sampOpts);
                if (i - bStart != mouseOverTileIndex) {
                    multiLabels[i].mouseOver = false;
                }

                Drawing::drawLabel(opts, canvas, rect, multiLabels[i], fonts, seenLabels, srtLabels);
            }
            ++i;
        }

        sContext->flush();
        glfwSwapBuffers(window);
        redraw = false;
    }

    void GwPlot::drawSurfaceGpu(SkCanvas *canvas) {
        canvas->drawPaint(opts.theme.bgPaint);
        setGlfwFrameBufferSize();
        processBam();
        setScaling();
        if (opts.coverage) {
            Drawing::drawCoverage(opts, collections, canvas, fonts, covY, refSpace);
        }
        Drawing::drawBams(opts, collections, canvas, yScaling, fonts, linked, opts.link_op, refSpace);
        Drawing::drawRef(opts, regions, fb_width, canvas, fonts, refSpace, (float)regions.size());
        Drawing::drawBorders(opts, fb_width, fb_height, canvas, regions.size(), bams.size(), totalTabixY, tabixY, tracks.size());
        Drawing::drawTracks(opts, fb_width, fb_height, canvas, totalTabixY, tabixY, tracks, regions, fonts);
    }

    void GwPlot::runDraw(SkCanvas *canvas) {
        fetchRefSeqs();
        processBam();
        setScaling();
        canvas->drawPaint(opts.theme.bgPaint);
        if (opts.coverage) {
            Drawing::drawCoverage(opts, collections, canvas, fonts, covY, refSpace);
        }
        Drawing::drawBams(opts, collections, canvas, yScaling, fonts, linked, opts.link_op, refSpace);
        Drawing::drawRef(opts, regions, fb_width, canvas, fonts, refSpace, (float)regions.size());
        Drawing::drawBorders(opts, fb_width, fb_height, canvas, regions.size(), bams.size(), totalTabixY, tabixY, tracks.size());
        Drawing::drawTracks(opts, fb_width, fb_height, canvas, totalTabixY, tabixY, tracks, regions, fonts);
    }

    void imageToPng(sk_sp<SkImage> &img, std::string &path) {
        if (!img) { return; }
        sk_sp<SkData> png(img->encodeToData());
        if (!png) { return; }
        SkFILEWStream out(path.c_str());
        (void)out.write(png->data(), png->size());
    }

    sk_sp<SkImage> GwPlot::makeImage() {
        setScaling();
        sk_sp<SkSurface> rasterSurface = SkSurface::MakeRasterN32Premul(fb_width, fb_height);
        SkCanvas *canvas = rasterSurface->getCanvas();
        runDraw(canvas);
        sk_sp<SkImage> img(rasterSurface->makeImageSnapshot());
        return img;
    }
}

