/*******************************************************************************

  Copyright (c) 2017, Honda Research Institute Europe GmbH.
  All rights reserved.

  Redistribution and use in source and binary forms, with or without
  modification, are permitted provided that the following conditions are met:

  1. Redistributions of source code must retain the above copyright notice,
     this list of conditions and the following disclaimer.

  2. Redistributions in binary form must reproduce the above copyright notice,
     this list of conditions and the following disclaimer in the documentation
     and/or other materials provided with the distribution.

  3. All advertising materials mentioning features or use of this software
     must display the following acknowledgement: This product includes
     software developed by the Honda Research Institute Europe GmbH.

  4. Neither the name of the copyright holder nor the names of its
     contributors may be used to endorse or promote products derived from
     this software without specific prior written permission.

  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDER "AS IS" AND ANY EXPRESS OR
  IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
  OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
  IN NO EVENT SHALL THE COPYRIGHT HOLDER BE LIABLE FOR ANY DIRECT, INDIRECT,
  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
  LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA,
  OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
  LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
  NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE,
  EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

*******************************************************************************/

#include "RcsViewer.h"
#include "Rcs_graphicsUtils.h"

#include <Rcs_macros.h>
#include <Rcs_timer.h>
#include <KeyCatcherBase.h>
#include <Rcs_Vec3d.h>
#include <Rcs_VecNd.h>

#include <osgDB/ReadFile>
#include <osgDB/WriteFile>
#include <osgViewer/ViewerEventHandlers>
#include <osgUtil/Optimizer>
#include <osg/StateSet>
#include <osg/PolygonMode>
#include <osgShadow/ShadowMap>
#include <osgFX/Cartoon>
#include <osgGA/TrackballManipulator>

#include <iostream>


#if !defined (_MSC_VER)

#include <sys/wait.h>
#include <unistd.h>

static pid_t forkProcess(const char* command)
{
    pid_t pid = fork();

    if (pid == 0)
    {
        execl("/bin/bash", "bash", "-c", command, NULL);
        perror("execl");
        exit(1);
    }

    return pid;
}
#endif

/*******************************************************************************
 * Keyboard handler for default keys. The default manipulator is extended so
 * that the default space behavior (default camera pose) is disabled, and that
 * the mouse does not move the tracker once Shift-L is pressed. This allows to
 * implement a mouse spring.
 ******************************************************************************/
class RcsManipulator : public osgGA::TrackballManipulator
{
public:
    RcsManipulator() : osgGA::TrackballManipulator(), leftShiftPressed(false)
    {
    }

    ~RcsManipulator()
    {
    }

    virtual bool handle(const osgGA::GUIEventAdapter& ea,
                        osgGA::GUIActionAdapter& aa)
    {
        bool spacePressed = false;

        switch (ea.getEventType())
        {
        case (osgGA::GUIEventAdapter::KEYDOWN):
        {
            if (ea.getKey() == osgGA::GUIEventAdapter::KEY_Shift_L)
            {
                this->leftShiftPressed = true;
            }
            else if (ea.getKey() == osgGA::GUIEventAdapter::KEY_Space)
            {
                spacePressed = true;
            }

            break;
        }

        case (osgGA::GUIEventAdapter::KEYUP):
        {
            if (ea.getKey() == osgGA::GUIEventAdapter::KEY_Shift_L)
            {
                this->leftShiftPressed = false;
            }
            break;
        }

        default:
        {
        }
        }   // switch(...)

        if ((this->leftShiftPressed==true) || (spacePressed==true))
        {
            return false;
        }

        return osgGA::TrackballManipulator::handle(ea, aa);
    }



    bool leftShiftPressed;
};

/*******************************************************************************
 * Keyboard handler for default keys.
 ******************************************************************************/
namespace Rcs
{

struct ViewerEventData : public osg::Referenced
{
    enum EventType
    {
        AddNode = 0,
        AddChildNode,
        AddEventHandler,
        RemoveNode,
        RemoveNamedNode,
        RemoveChildNode,
        RemoveAllNodes,
        SetCameraTransform,
        None
    };

    ViewerEventData(EventType type) : eType(type)
    {
        init(type, "No arguments");
    }

    ViewerEventData(osg::ref_ptr<osg::Node> node_, EventType type) :
        node(node_), eType(type)
    {
        init(type, node->getName());
    }

    ViewerEventData(const HTr* transform, EventType type) : eType(type)
    {
        HTr_copy(&trf, transform);
        init(type, "Transform");
    }

    ViewerEventData(std::string nodeName, EventType type) :
        childName(nodeName), eType(type)
    {
        init(type, nodeName);
    }

    ViewerEventData(osg::ref_ptr<osg::Node> parent_,
                    osg::ref_ptr<osg::Node> node_, EventType type) :
        parent(parent_), node(node_), eType(type)
    {
        init(type, node->getName());
    }

    ViewerEventData(osg::ref_ptr<osgGA::GUIEventHandler> eHandler,
                    EventType type) :
        eventHandler(eHandler), eType(type)
    {
        init(type, "osgGA::GUIEventHandler");
    }

    ViewerEventData(osg::Node* parent_, std::string childName_, EventType type) :
        parent(parent_), childName(childName_), eType(type)
    {
        init(type, "osgGA::GUIEventHandler");
    }

    void init(EventType type, std::string comment)
    {
        RLOG(5, "Creating ViewerEventData %d: %s", userEventCount, comment.c_str());
        userEventCount++;
    }

    ~ViewerEventData()
    {
        userEventCount--;
        RLOG(5, "Deleting ViewerEventData - now %d events", userEventCount);
    }


    osg::ref_ptr<osg::Node> parent;
    osg::ref_ptr<osg::Node> node;
    std::string childName;
    osg::ref_ptr<osgGA::GUIEventHandler> eventHandler;
    EventType eType;
    HTr trf;
    static int userEventCount;
};

int Rcs::ViewerEventData::userEventCount = 0;

class KeyHandler : public osgGA::GUIEventHandler
{
public:

    KeyHandler(Rcs::Viewer* viewer) : _viewer(viewer),
        _video_capture_process(-1)
    {
        RCHECK(_viewer);

        KeyCatcherBase::registerKey("0-9", "Set Rcs debug level", "Viewer");
        KeyCatcherBase::registerKey("w", "Toggle wireframe mode", "Viewer");
        KeyCatcherBase::registerKey("s", "Cycle between shadow modes", "Viewer");
        KeyCatcherBase::registerKey("R", "Toggle cartoon mode", "Viewer");
#if !defined(_MSC_VER)
        KeyCatcherBase::registerKey("M", "Toggle video capture", "Viewer");
#endif
        KeyCatcherBase::registerKey("F11", "Print camera transform", "Viewer");
    }

    ~KeyHandler()
    {
        if (_video_capture_process >= 0)
        {
            toggleVideoCapture();
        }
    }

    virtual bool handle(const osgGA::GUIEventAdapter& ea,
                        osgGA::GUIActionAdapter& aa)
    {
        return _viewer->handle(ea, aa);
    }

    bool toggleVideoCapture()
    {
        bool captureRunning = false;

#if !defined(_MSC_VER)
        if (_video_capture_process >= 0)
        {
            // Stop video taking
            kill(_video_capture_process, SIGINT);
            waitpid(_video_capture_process, NULL, 0);
            _video_capture_process = -1;
        }
        else
        {
            // movie taken using avconv x11grab

            // get the first window
            osgViewer::ViewerBase::Windows windows;
            _viewer->viewer->getWindows(windows, true);

            if (!windows.empty())
            {
                int x = 0;
                int y = 0;
                int w = 0;
                int h = 0;

                windows[0]->getWindowRectangle(x, y, w, h);

                static unsigned int movie_number = 1;

                RMSG("Start capturing: (%d, %d) %dx%d", x, y, w, h);
                std::stringstream cmd;
                cmd << "ffmpeg -y -f x11grab -r 25 -s "
                    << w << "x" << h
                    << " -i " << getenv("DISPLAY") << "+"
                    << x << "," << y
                    << " -crf 20 -r 25 -c:v libx264 -c:a n"
                    << " /tmp/movie_" << movie_number++ << ".mp4";

                // cmd << "avconv -y -f x11grab -r 25 -s "
                //     << w << "x" << h
                //     << " -i " << getenv("DISPLAY") << "+"
                //     << x << "," << y
                //     << " -crf 20 -r 25 -c:v libx264 -c:a n"
                //     << " /tmp/movie_" << movie_number++ << ".mp4";

                _video_capture_process = forkProcess(cmd.str().c_str());
                captureRunning = true;
            }
        }
#endif

        return captureRunning;
    }

private:

    Rcs::Viewer* _viewer;
    pid_t _video_capture_process;
};

/*******************************************************************************
 * Viewer class.
 ******************************************************************************/
Viewer::Viewer() :
    fps(0.0), mouseX(0.0), mouseY(0.0), normalizedMouseX(0.0),
    normalizedMouseY(0.0), mtxFrameUpdate(NULL), threadRunning(false),
    updateFreq(25.0), initialized(false), wireFrame(false), shadowsEnabled(false),
    llx(0), lly(0), sizeX(640), sizeY(480), cartoonEnabled(false),
    threadStopped(true), leftMouseButtonPressed(false),
    rightMouseButtonPressed(false), title("RcsViewer")
{
    // Check if logged in remotely
    const char* sshClient = getenv("SSH_CLIENT");
    bool fancy = true;

    if (sshClient != NULL)
    {
        if (strlen(sshClient) > 0)
        {
            RLOGS(4, "Remote login detected - simple viewer settings");
            fancy = false;
        }
    }

    create(fancy, fancy);

    RLOG(5, "Done constructor of viewer");
}

/*******************************************************************************
 * Viewer class.
 ******************************************************************************/
Viewer::Viewer(bool fancy, bool startupWithShadow) :
    fps(0.0), mouseX(0.0), mouseY(0.0), normalizedMouseX(0.0),
    normalizedMouseY(0.0), mtxFrameUpdate(NULL), threadRunning(false),
    updateFreq(25.0), initialized(false), wireFrame(false), shadowsEnabled(false),
    llx(0), lly(0), sizeX(640), sizeY(480), cartoonEnabled(false),
    threadStopped(true), leftMouseButtonPressed(false),
    rightMouseButtonPressed(false), title("RcsViewer")
{
    create(fancy, startupWithShadow);

    RLOG(5, "Done constructor of viewer");
}

/*******************************************************************************
 * Destructor.
 ******************************************************************************/
Viewer::~Viewer()
{
    stopUpdateThread();
    pthread_mutex_destroy(&this->mtxEventLoop);
}

/*******************************************************************************
 * Initlalization method.
 ******************************************************************************/
void Viewer::create(bool fancy, bool startupWithShadow)
{
#if defined(_MSC_VER)
    llx = 12;
    lly = 31;
#endif

    const char* forceSimple = getenv("RCSVIEWER_SIMPLEGRAPHICS");

    if (forceSimple)
    {
        fancy = false;
        startupWithShadow = false;
    }

    pthread_mutex_init(&this->mtxEventLoop, NULL);
    this->shadowsEnabled = startupWithShadow;

    // Rotate loaded file nodes to standard coordinate conventions
    // (z: up, x: forward)
    osg::ref_ptr<osgDB::ReaderWriter::Options> options;
    options = new osgDB::ReaderWriter::Options;
    options->setOptionString("noRotation");
    osgDB::Registry::instance()->setOptions(options.get());

    this->viewer = new osgViewer::Viewer();

    // Mouse manipulator (needs to go before event handler)
    osg::ref_ptr<osgGA::TrackballManipulator> trackball = new RcsManipulator();
    viewer->setCameraManipulator(trackball.get());

    // Handle some default keys (see handler above)
    this->keyHandler = new KeyHandler(this);
    viewer->addEventHandler(this->keyHandler.get());

    // Root node (instead of a Group we create an Cartoon node for optional
    // cell shading)
    if (fancy)
    {
        this->rootnode = new osgFX::Cartoon;
        dynamic_cast<osgFX::Effect*>(rootnode.get())->setEnabled(false);
    }
    else
    {
        this->rootnode = new osg::Group;
    }

    rootnode->setName("rootnode");

    // Light grayish green universe
    this->clearNode = new osg::ClearNode;
    this->clearNode->setClearColor(colorFromString("LIGHT_GRAYISH_GREEN"));
    this->rootnode->addChild(this->clearNode.get());

    // Light model: We switch off the default viewer light, and configure two
    // light sources. The sunlight shines down from 10m. Another light source
    // moves with the camera, so that there are no dark spots whereever
    // the mouse manipulator moves to.

    // Disable default light
    rootnode->getOrCreateStateSet()->setMode(GL_LIGHT0, osg::StateAttribute::OFF);

    // Light source that moves with the camera
    this->cameraLight = new osg::LightSource;
    cameraLight->getLight()->setLightNum(1);
    cameraLight->getLight()->setPosition(osg::Vec4(0.0, 0.0, 10.0, 1.0));
    cameraLight->getLight()->setSpecular(osg::Vec4(1.0, 1.0, 1.0, 1.0));
    rootnode->addChild(cameraLight.get());
    rootnode->getOrCreateStateSet()->setMode(GL_LIGHT1, osg::StateAttribute::ON);

    // Light source that shines down
    osg::ref_ptr<osg::LightSource> sunlight = new osg::LightSource;
    sunlight->getLight()->setLightNum(2);
    sunlight->getLight()->setPosition(osg::Vec4(0.0, 0.0, 10.0, 1.0));
    rootnode->addChild(sunlight.get());
    rootnode->getOrCreateStateSet()->setMode(GL_LIGHT2, osg::StateAttribute::ON);

    // Shadow map scene. We use the sunlight to case shadows.
    this->shadowScene = new osgShadow::ShadowedScene;
    osg::ref_ptr<osgShadow::ShadowMap> sm = new osgShadow::ShadowMap;
    sm->setTextureSize(osg::Vec2s(2048, 2048));
    sm->setLight(sunlight->getLight());
    sm->setPolygonOffset(osg::Vec2(-0.7, 0.0));
    sm->setAmbientBias(osg::Vec2(0.7, 0.3));   // values need to sum up to 1.0

    shadowScene->setShadowTechnique(sm.get());
    shadowScene->addChild(rootnode.get());
    shadowScene->setReceivesShadowTraversalMask(ReceivesShadowTraversalMask);
    shadowScene->setCastsShadowTraversalMask(CastsShadowTraversalMask);


    // Change the threading model. The default threading model is
    // osgViewer::Viewer::CullThreadPerCameraDrawThreadPerContext.
    // This leads to problems with multi-threaded updates (HUD).

    if (forceSimple)
    {
        viewer->setThreadingModel(osgViewer::Viewer::SingleThreaded);
    }
    else
    {
        viewer->setThreadingModel(osgViewer::Viewer::CullDrawThreadPerContext);
    }

    // Create viewer in a window
    if (fancy == false)
    {
        viewer->setSceneData(rootnode.get());
    }
    else
    {
        // Set anti-aliasing
        osg::ref_ptr<osg::DisplaySettings> ds = new osg::DisplaySettings;
        ds->setNumMultiSamples(4);
        viewer->setDisplaySettings(ds.get());
        viewer->setSceneData(startupWithShadow?shadowScene.get():rootnode.get());
    }

    // Disable small feature culling to avoid problems with drawing single points
    // as they have zero bounding box size
    viewer->getCamera()->setCullingMode(viewer->getCamera()->getCullingMode() &
                                        ~osg::CullSettings::SMALL_FEATURE_CULLING);

    setCameraHomePosition(osg::Vec3d(4.0,  3.5, 3.0),
                          osg::Vec3d(0.0, -0.2, 0.8),
                          osg::Vec3d(0.0, 0.05, 1.0));

    KeyCatcherBase::registerKey("F10", "Toggle full screen", "Viewer");
    osg::ref_ptr<osgViewer::WindowSizeHandler> wsh;
    wsh = new osgViewer::WindowSizeHandler;
    wsh->setKeyEventToggleFullscreen(osgGA::GUIEventAdapter::KEY_F10);
    viewer->addEventHandler(wsh.get());

    KeyCatcherBase::registerKey("F9", "Toggle continuous screenshots", "Viewer");
    KeyCatcherBase::registerKey("F8", "Take screenshot(s)", "Viewer");

    osg::ref_ptr<osgViewer::ScreenCaptureHandler::WriteToFile> scrw;
    scrw = new osgViewer::ScreenCaptureHandler::WriteToFile("screenshot", "png");

    osg::ref_ptr<osgViewer::ScreenCaptureHandler> capture;
    capture = new osgViewer::ScreenCaptureHandler(scrw.get());
    capture->setKeyEventToggleContinuousCapture(osgGA::GUIEventAdapter::KEY_F9);
    capture->setKeyEventTakeScreenShot(osgGA::GUIEventAdapter::KEY_F8);
    viewer->addEventHandler(capture.get());

    KeyCatcherBase::registerKey("z", "Toggle on-screen stats", "Viewer");
    KeyCatcherBase::registerKey("Z", "Print viewer stats to console", "Viewer");
    osg::ref_ptr<osgViewer::StatsHandler> stats = new osgViewer::StatsHandler;
    stats->setKeyEventTogglesOnScreenStats('z');
    stats->setKeyEventPrintsOutStats('Z');
    viewer->addEventHandler(stats.get());
}

/*******************************************************************************
 * Add a node to the root node.
 ******************************************************************************/
bool Viewer::setWindowSize(unsigned int llx_,     // lower left x
                           unsigned int lly_,     // lower left y
                           unsigned int sizeX_,   // size in x-direction
                           unsigned int sizeY_)
{
    if (isInitialized() == true)
    {
        RLOG(1, "The window size can't be changed after launching the viewer "
                "window");
        return false;
    }

    this->llx = llx_;
    this->lly = lly_;
    this->sizeX = sizeX_;
    this->sizeY = sizeY_;

    return true;
}

/*******************************************************************************
 * \ţodo: In case the viewer is bout to be realized, we might get into the
 *        realized==false branch. If it then gets realized, we get a
 *        concurrency problem. Can this ever happen? Does it make sense to
 *        handle this?
 ******************************************************************************/
void Viewer::add(osgGA::GUIEventHandler* eventHandler)
{
    RLOG(5, "Adding event handler");
    if (viewer->isRealized())
    {
        osg::ref_ptr<ViewerEventData> ev;
        ev = new ViewerEventData(eventHandler, ViewerEventData::AddEventHandler);
        viewer->getEventQueue()->userEvent(ev.get());
    }
    else
    {
        viewer->addEventHandler(eventHandler);
    }
}

/*******************************************************************************
 *
 ******************************************************************************/
void Viewer::addInternal(osgGA::GUIEventHandler* eventHandler)
{
    viewer->addEventHandler(eventHandler);
}

/*******************************************************************************
 * Add a node to the root node.
 ******************************************************************************/
void Viewer::add(osg::Node* node)
{
    if (viewer->isRealized())
    {
        osg::ref_ptr<ViewerEventData> ev;
        osg::ref_ptr<osg::Node> refNode(node);
        ev = new ViewerEventData(refNode, ViewerEventData::AddNode);
        RLOG(5, "Adding node %s to eventqueue", node->getName().c_str());
        viewer->getEventQueue()->userEvent(ev.get());
    }
    else
    {
        RLOG(5, "Adding node %s directly", node->getName().c_str());
        addInternal(node);
    }
}

/*******************************************************************************
 * Add a node to the root node.
 ******************************************************************************/
bool Viewer::addInternal(osg::Node* node)
{
    osg::Camera* newHud = dynamic_cast<osg::Camera*>(node);

    // If it's a camera, it needs a graphics context. This doesn't exist right
    // after construction, therefore in that case we push all cameras on a
    // vector and add them later. In case the graphics context exists, we can
    // directly add it.
    if (newHud != NULL)
    {
        osgViewer::Viewer::Windows windows;
        viewer->getWindows(windows);

        if (windows.empty())
        {
            this->hud.push_back(newHud);
        }
        else
        {
            newHud->setGraphicsContext(windows[0]);
            newHud->setViewport(0, 0, windows[0]->getTraits()->width,
                    windows[0]->getTraits()->height);
            viewer->addSlave(newHud, false);
        }

        return true;
    }

    bool success = false;

    if (node != NULL)
    {
        success = this->rootnode->addChild(node);
    }
    else
    {
        RLOG(1, "Failed to add osg::Node - node is NULL!");
    }

    return success;
}

/*******************************************************************************
 * Add a node to the parent node.
 ******************************************************************************/
void Viewer::add(osg::Node* parent, osg::Node* child)
{
    if (viewer->isRealized())
    {
        osg::ref_ptr<ViewerEventData> ev;
        ev = new ViewerEventData(parent, child, ViewerEventData::AddChildNode);
        viewer->getEventQueue()->userEvent(ev.get());
    }
    else
    {
        addInternal(parent, child);
    }
}

/*******************************************************************************
 * Add a node to the parent node.
 ******************************************************************************/
bool Viewer::addInternal(osg::Node* parent, osg::Node* child)
{
    osg::Group* grp = dynamic_cast<osg::Group*>(parent);
    if (!grp)
    {
        RLOG(1, "Can't add child to node (%s) other than derived from osg::Group",
             parent->getName().c_str());
        return false;
    }

    grp->addChild(child);
    return true;
}

/*******************************************************************************
 * Removes a node from the scene graph.
 ******************************************************************************/
void Viewer::removeNode(osg::Node* node)
{
    if (viewer->isRealized())
    {
        osg::ref_ptr<ViewerEventData> ev;
        ev = new ViewerEventData(node, ViewerEventData::RemoveNode);
        viewer->getEventQueue()->userEvent(ev.get());
    }
    else
    {
        removeInternal(node);
    }
}

/*******************************************************************************
 * Removes a node from the scene graph.
 ******************************************************************************/
void Viewer::removeNode(std::string nodeName)
{
    RLOG_CPP(5, "Removing node " << nodeName);
    if (viewer->isRealized())
    {
        osg::ref_ptr<ViewerEventData> ev;
        ev = new ViewerEventData(nodeName, ViewerEventData::RemoveNamedNode);
        viewer->getEventQueue()->userEvent(ev.get());
    }
    else
    {
        removeInternal(nodeName);
    }
}

/*******************************************************************************
 * Removes all nodes with a given name from a parent node.
 ******************************************************************************/
void Viewer::removeNode(osg::Node* parent, std::string child)
{
    RLOG_CPP(5, "Removing node " << child << " of parent " << parent->getName());
    if (viewer->isRealized())
    {
        osg::ref_ptr<ViewerEventData> ev;
        ev = new ViewerEventData(parent, child, ViewerEventData::RemoveChildNode);
        viewer->getEventQueue()->userEvent(ev.get());
    }
    else
    {
        int numNodes = removeInternal(parent, child);
        RLOG_CPP(5, "Removed " << numNodes << " children with name " << child
                 << " from parent " << parent->getName());
    }
}

/*******************************************************************************
 *
 ******************************************************************************/
void Viewer::removeNodes()
{
    if (viewer->isRealized())
    {
        osg::ref_ptr<ViewerEventData> ev;
        ev = new ViewerEventData(ViewerEventData::RemoveAllNodes);
        viewer->getEventQueue()->userEvent(ev.get());
    }
    else
    {
        removeAllNodesInternal();
    }
}

/*******************************************************************************
 *
 ******************************************************************************/
void Viewer::setCameraTransform(const HTr* A_CI)
{
    if (viewer->isRealized())
    {
        osg::ref_ptr<ViewerEventData> ev;
        ev = new ViewerEventData(A_CI, ViewerEventData::SetCameraTransform);
        viewer->getEventQueue()->userEvent(ev.get());
    }
    else
    {
        osg::Matrix vm = viewMatrixFromHTr(A_CI);
        viewer->getCameraManipulator()->setByInverseMatrix(vm);
    }
}

/*******************************************************************************
 *
 ******************************************************************************/
void Viewer::setCameraTransform(double x, double y, double z,
                                double thx, double thy, double thz)
{
    HTr A_CI;
    double x6[6];
    VecNd_set6(x6, x, y, z, thx, thy, thz);
    HTr_from6DVector(&A_CI, x6);
    setCameraTransform(&A_CI);
}

/*******************************************************************************
 * Removes all nodes with the given name from the rootNode
 ******************************************************************************/
int Viewer::removeInternal(std::string nodeName)
{
    int nnd = 0;
    osg::Node* ndi;

    do
    {
        ndi = getNode(nodeName);
        if (ndi)
        {
            bool success = removeInternal(ndi);

            if (success)
            {
                nnd++;
            }
            else
            {
                RLOG(4, "Failed to remove node %s at iteration %d",
                     nodeName.c_str(), nnd);
            }
        }

    }
    while (ndi);

    RLOG(5, "Removed %d nodes with name %s from the viewer",
         nnd, nodeName.c_str());

    return nnd;
}

/*******************************************************************************
 * Removes a node from the scene graph.
 ******************************************************************************/
bool Viewer::removeInternal(osg::Node* node)
{
    if (node == NULL)
    {
        RLOG(1, "Node is NULL - can't be deleted");
        return false;
    }

    osg::Camera* hud = dynamic_cast<osg::Camera*>(node);

    if (hud != NULL)
    {
        osg::View::Slave* slave = viewer->findSlaveForCamera(hud);

        if (slave != NULL)
        {
            RLOG(4, "Hud can't be deleted - is not part of the scene graph");
            return false;
        }

        // We are a bit pedantic and check that the camera is not the
        // viewer's camera.
        unsigned int si = viewer->findSlaveIndexForCamera(hud);
        unsigned int ci = viewer->findSlaveIndexForCamera(viewer->getCamera());

        if (ci != si)
        {
            viewer->removeSlave(si);
            RLOG(5, "Hud successully deleted");
        }
        else
        {
            RLOG(1, "Cannot remove the viewer's camera");
            return false;
        }

        return true;
    }

    osg::Node::ParentList parents = node->getParents();
    size_t nDeleted = 0;

    for (size_t i=0; i<parents.size(); ++i)
    {
        nDeleted++;
        parents[i]->removeChild(node);
    }

    if (nDeleted == 0)
    {
        RLOG(1, "Node can't be deleted - is not part of the scene graph");
        return false;
    }

    return true;
}

/*******************************************************************************
 * Search through the parent node. We do this in a while loop to remove all
 * nodes with the same name
 ******************************************************************************/
int Viewer::removeInternal(osg::Node* parent, std::string nodeName)
{
    osg::Node* toRemove = findNamedNodeRecursive(parent, nodeName);
    int nnd = 0;

    while (toRemove)
    {
        removeInternal(toRemove);
        toRemove = findNamedNodeRecursive(parent, nodeName);
        nnd++;
    }

    return nnd;
}

/*******************************************************************************
 *
 ******************************************************************************/
int Viewer::removeAllNodesInternal()
{
    int nDeleted = rootnode->getNumChildren();
    rootnode->removeChildren(0, nDeleted);
    this->rootnode->addChild(this->clearNode.get());
    RLOG_CPP(5, "Removing all " << nDeleted << " nodes");

    return nDeleted;
}

/*******************************************************************************
 * Sets the update frequency in [Hz].
 ******************************************************************************/
void Viewer::setUpdateFrequency(double Hz)
{
    this->updateFreq = Hz;
}

/*******************************************************************************
 * Returns the update frequency in [Hz].
 ******************************************************************************/
double Viewer::updateFrequency() const
{
    return this->updateFreq;
}

/*******************************************************************************
 * Sets the camera position and viewing direction
 * First vector is where camera is, Second vector is where the
 * camera points to, the up vector is set internally to always stay upright
 ******************************************************************************/
void Viewer::setCameraHomePosition(const osg::Vec3d& eye,
                                   const osg::Vec3d& center,
                                   const osg::Vec3d& up)
{
    viewer->getCameraManipulator()->setHomePosition(eye, center, up);
    viewer->home();
}

/*******************************************************************************
 *
 ******************************************************************************/
void Viewer::resetView()
{
    viewer->getCamera()->setProjectionMatrix(this->startView);
}

/*******************************************************************************
 *
 ******************************************************************************/
void Viewer::setCameraHomePosition(const HTr* A_CI)
{
    osg::Vec3d eye(A_CI->org[0], A_CI->org[1], A_CI->org[2]);

    osg::Vec3d center(A_CI->org[0] + A_CI->rot[2][0],
            A_CI->org[1] + A_CI->rot[2][1],
            A_CI->org[2] + A_CI->rot[2][2]);

    osg::Vec3d up(-A_CI->rot[1][0], -A_CI->rot[1][1], -A_CI->rot[1][2]);

    setCameraHomePosition(eye, center, up);
}

/*******************************************************************************
 * Sets the title of the viewer windows
 ******************************************************************************/
void Viewer::setTitle(const std::string& title)
{
    // We remember the title since if the window is not realized (yet),
    // we can still set the title once the window is realized.
    this->title = title;

    if(isRealized())
    {
        lock();

        // We set the title for all windows, but only one window should be created anyways
        osgViewer::ViewerBase::Windows windows;
        this->viewer->getWindows(windows);
        for(osgViewer::ViewerBase::Windows::iterator window = windows.begin(); window != windows.end(); window++)
        {
            (*window)->setWindowName(title);
        }

        unlock();
    }
}

/*******************************************************************************
 *
 ******************************************************************************/
void Viewer::getCameraTransform(HTr* A_CI) const
{
    osg::Matrix matrix = viewer->getCamera()->getViewMatrix();
    HTr_fromViewMatrix(matrix, A_CI);
}

/*******************************************************************************
 *
 ******************************************************************************/
osg::Node* Viewer::getNodeUnderMouse(double I_mouseCoords[3])
{
    return Rcs::getNodeUnderMouse<osg::Node*>(*this->viewer.get(),
                                              mouseX, mouseY,
                                              I_mouseCoords);
}

/*******************************************************************************
 *
 ******************************************************************************/
osg::Node* Viewer::getNode(std::string nodeName)
{
    return findNamedNodeRecursive(rootnode, nodeName);
}

/*******************************************************************************
 *
 ******************************************************************************/
double Viewer::getFieldOfView() const
{
    double fovy, aspectRatio, zNear, zFar;
    viewer->getCamera()->getProjectionMatrixAsPerspective(fovy, aspectRatio,
                                                          zNear, zFar);
    return fovy;
}

/*******************************************************************************
 *
 ******************************************************************************/
void Viewer::setFieldOfView(double fovy)
{
    double fovy_old, aspectRatio, zNear, zFar;
    viewer->getCamera()->getProjectionMatrixAsPerspective(fovy_old, aspectRatio,
                                                          zNear, zFar);
    viewer->getCamera()->setProjectionMatrixAsPerspective(fovy, aspectRatio,
                                                          zNear, zFar);
}

/*******************************************************************************
 * Defaults are:
 * fov_org = 29.148431   aspectRatio_org = 1.333333
 * znear=1.869018   zfar=10.042613
 ******************************************************************************/
void Viewer::setFieldOfView(double fovWidth, double fovHeight)
{
    double fovy_old, aspectRatio, zNear, zFar;
    viewer->getCamera()->getProjectionMatrixAsPerspective(fovy_old, aspectRatio,
                                                          zNear, zFar);
    viewer->getCamera()->setProjectionMatrixAsPerspective(fovWidth,
                                                          fovWidth/fovHeight,
                                                          zNear, zFar);
}

/*******************************************************************************
 * Runs the viewer in its own thread.
 ******************************************************************************/
void* Viewer::ViewerThread(void* arg)
{
    Rcs::Viewer* viewer = static_cast<Rcs::Viewer*>(arg);

    if (viewer->isThreadRunning() == true)
    {
        RLOG(1, "Viewer thread is already running");
        return NULL;
    }

    viewer->lock();
    viewer->init();
    viewer->threadRunning = true;
    viewer->unlock();

    while (viewer->isThreadRunning() == true)
    {
        viewer->frame();
        unsigned long dt = (unsigned long)(1.0e6/viewer->updateFrequency());
        Timer_usleep(dt);
    }

    return NULL;
}

/*******************************************************************************
 *
 ******************************************************************************/
void Viewer::runInThread(pthread_mutex_t* mutex)
{
    this->mtxFrameUpdate = mutex;
    threadStopped = false;
    pthread_create(&frameThread, NULL, ViewerThread, (void*) this);

    // Wait until the class has been initialized
    while (!isInitialized())
    {
        Timer_usleep(10000);
    }

    // ... and realized
    while (!isRealized())
    {
        Timer_usleep(10000);
    }

    // Set the window title
    setTitle(this->title);
}

/*******************************************************************************
 * For true, displays all nodes in wireframe, otherwise in solid
 ******************************************************************************/
void Viewer::displayWireframe(bool wf)
{
    this->wireFrame = wf;
    osg::ref_ptr<osg::StateSet> pStateSet = rootnode->getOrCreateStateSet();

    if (wf == true)
    {
        pStateSet->setAttribute(new osg::PolygonMode
                                (osg::PolygonMode::FRONT_AND_BACK,
                                 osg::PolygonMode::LINE));
    }
    else
    {
        pStateSet->setAttribute(new osg::PolygonMode
                                (osg::PolygonMode::FRONT_AND_BACK,
                                 osg::PolygonMode::FILL));
    }

}

/*******************************************************************************
 * Toggles between solid and wireframe display
 ******************************************************************************/
void Viewer::toggleWireframe()
{
    displayWireframe(!this->wireFrame);
}

/*******************************************************************************
 * Switches between shadow casting modes.
 ******************************************************************************/
void Viewer::setShadowEnabled(bool enable)
{
    osg::Matrix lastViewMatrix = viewer->getCameraManipulator()->getMatrix();

    if (enable==false)
    {
        RLOG(3, "Shadows off");
        if (this->shadowsEnabled == true)
        {
            viewer->setSceneData(rootnode.get());
        }
        this->shadowsEnabled = false;
    }
    else
    {
        RLOG(3, "Shadows on");
        if (this->shadowsEnabled == false)
        {
            viewer->setSceneData(shadowScene.get());
        }
        this->shadowsEnabled = true;
    }

    viewer->getCameraManipulator()->setByMatrix(lastViewMatrix);
}

/*******************************************************************************
 * Renders the scene in cartoon mode.
 ******************************************************************************/
void Viewer::setCartoonEnabled(bool enabled)
{
    osgFX::Effect* cartoon = dynamic_cast<osgFX::Effect*>(rootnode.get());

    if (!cartoon)
    {
        return;
    }

    if (enabled == true)
    {
        setShadowEnabled(false);
    }

    cartoon->setEnabled(enabled);
}

/*******************************************************************************
 * Renders the scene in cartoon mode.
 ******************************************************************************/
void Viewer::setBackgroundColor(const char* color)
{
    this->clearNode->setClearColor(colorFromString(color));
}

/*******************************************************************************
 *
 ******************************************************************************/
void Viewer::frame()
{
    if (isInitialized() == false)
    {
        init();
    }

    double dtFrame = Timer_getSystemTime();

    lock();
    viewer->frame();
    unlock();

    dtFrame = Timer_getSystemTime() - dtFrame;
    this->fps = 0.9*this->fps + 0.1*(1.0/dtFrame);
}

/*******************************************************************************
  This initialization function needs to be called from the thread that also
  calls the osg update traversals. That's why it is separated from the
  constructor. Otherwise, it leads to problems under msvc.
*******************************************************************************/
void Viewer::init()
{
    if (isInitialized() == true)
    {
        return;
    }

    viewer->setUpViewInWindow(llx, lly, sizeX, sizeY);

    // Stop listening to ESC key, cause it doesn't end RCS properly
    viewer->setKeyEventSetsDone(0);
    viewer->realize();

    // Add all HUD's after creation of the window
    osgViewer::Viewer::Windows windows;
    viewer->getWindows(windows);

    if (windows.empty())
    {
        RLOG(1, "Failed to add HUD - no viewer window");
    }
    else
    {
        for (size_t i=0; i<hud.size(); i++)
        {
            hud[i]->setGraphicsContext(windows[0]);
            hud[i]->setViewport(0, 0, windows[0]->getTraits()->width,
                    windows[0]->getTraits()->height);
            viewer->addSlave(hud[i].get(), false);
        }

        hud.clear();
    }

    this->startView = viewer->getCamera()->getProjectionMatrix();
    this->initialized = true;
}

/*******************************************************************************
 *
 ******************************************************************************/
void Viewer::optimize()
{
    osgUtil::Optimizer optimizer;
    optimizer.optimize(rootnode);
}

/*******************************************************************************
 *
 ******************************************************************************/
void Viewer::setSceneData(osg::Node* node)
{
    viewer->setSceneData(node);
}

/*******************************************************************************
 *
 ******************************************************************************/
bool Viewer::isRealized() const
{
    return viewer->isRealized();
}

/*******************************************************************************
 *
 ******************************************************************************/
bool Viewer::isInitialized() const
{
    return this->initialized;
}

/*******************************************************************************
 *
 ******************************************************************************/
bool Viewer::isThreadRunning() const
{
    return this->threadRunning;
}

/*******************************************************************************
 *
 ******************************************************************************/
void Viewer::stopUpdateThread()
{
    if (threadRunning == false)
    {
        return;
    }

    this->threadRunning = false;
    pthread_join(frameThread, NULL);
    threadStopped = true;
}

/*******************************************************************************
 *
 ******************************************************************************/
osg::ref_ptr<osgViewer::Viewer> Viewer::getOsgViewer() const
{
    return this->viewer;
}

/*******************************************************************************
 *
 ******************************************************************************/
bool Viewer::lock() const
{

    if (this->mtxFrameUpdate!=NULL)
    {
        pthread_mutex_lock(this->mtxFrameUpdate);
        return true;
    }

    return false;
}

/*******************************************************************************
 *
 ******************************************************************************/
bool Viewer::unlock() const
{

    if (this->mtxFrameUpdate!=NULL)
    {
        pthread_mutex_unlock(this->mtxFrameUpdate);
        return true;
    }

    return false;
}

/*******************************************************************************
 *
 ******************************************************************************/
bool Viewer::toggleVideoRecording()
{
    return keyHandler->toggleVideoCapture();
}

/*******************************************************************************
 *
 ******************************************************************************/
void Viewer::getMouseTip(double tip[3]) const
{
    osg::Matrix vm = viewer->getCamera()->getViewMatrix();
    osg::Matrix pm = viewer->getCamera()->getProjectionMatrix();

    HTr A_CamI;
    getCameraTransform(&A_CamI);

    double planePt[3];
    Vec3d_add(planePt, A_CamI.org, A_CamI.rot[0]);

    Rcs::getMouseTip(vm, pm, normalizedMouseX, normalizedMouseY, planePt, tip);
}

/*******************************************************************************
 *
 ******************************************************************************/
void Viewer::handleUserEvents(const osg::Referenced* userEvent)
{
    RLOG(5, "Received user event");

    const ViewerEventData* ev = dynamic_cast<const ViewerEventData*>(userEvent);
    if (!ev)
    {
        RLOG(5, "User event not of type ViewerEventData - skipping");
        return;
    }

    switch (ev->eType)
    {
    case ViewerEventData::AddNode:
        if (ev->node.valid())
        {
            RLOG(5, "Adding node \"%s\"", ev->node->getName().c_str());
            addInternal(ev->node.get());
        }
        else
        {
            RLOG(5, "ViewerEventData::AddNode: Found invalid node");
        }
        break;

    case ViewerEventData::AddChildNode:
        RCHECK(ev->parent.valid());
        RCHECK(ev->node.valid());
        RLOG(5, "Adding node \"%s\"", ev->node->getName().c_str());
        addInternal(ev->parent.get(), ev->node.get());
        break;

    case ViewerEventData::RemoveNode:
        RCHECK(ev->node.valid());
        RLOG(5, "Removing node \"%s\"", ev->node->getName().c_str());
        removeInternal(ev->node.get());
        break;

    case ViewerEventData::RemoveNamedNode:
        RLOG(5, "Removing all nodes with name \"%s\"", ev->childName.c_str());
        removeInternal(ev->childName);
        break;

    case ViewerEventData::RemoveChildNode:
        RCHECK(ev->parent.valid());
        RLOG(5, "Removing child node \"%s\" from parent %s",
             ev->childName.c_str(), ev->parent->getName().c_str());
        removeInternal(ev->parent.get(), ev->childName);
        break;

    case ViewerEventData::RemoveAllNodes:
        removeAllNodesInternal();
        break;

    case ViewerEventData::AddEventHandler:
        RCHECK(ev->eventHandler.valid());
        RLOG(5, "Adding handler \"%s\"", ev->eventHandler->getName().c_str());
        viewer->addEventHandler(ev->eventHandler.get());
        break;

    case ViewerEventData::SetCameraTransform:
    {
        RLOG(5, "Setting camera transform");
        osg::Matrix vm = viewMatrixFromHTr(&ev->trf);
        viewer->getCameraManipulator()->setByInverseMatrix(vm);
    }
        break;

    default:
        RLOG(1, "Unknown event type %d", (int) ev->eType);
        break;
    }

}

/*******************************************************************************
 *
 ******************************************************************************/
bool Viewer::handle(const osgGA::GUIEventAdapter& ea,
                    osgGA::GUIActionAdapter& aa)
{
    switch (ea.getEventType())
    {

    /////////////////////////////////////////////////////////////////
    // User events triggered through classes API
    /////////////////////////////////////////////////////////////////
    case osgGA::GUIEventAdapter::USER:
    {
        handleUserEvents(ea.getUserData());
        break;
    }

        /////////////////////////////////////////////////////////////////
        // Frame update event
        /////////////////////////////////////////////////////////////////
    case (osgGA::GUIEventAdapter::FRAME):
    {
        this->mouseX = ea.getX();
        this->mouseY = ea.getY();
        this->normalizedMouseX = ea.getXnormalized();
        this->normalizedMouseY = ea.getYnormalized();

        if (cameraLight.valid())
        {
            HTr A_CI;
            getCameraTransform(&A_CI);
            osg::Vec4 lightpos;
            lightpos.set(A_CI.org[0], A_CI.org[1], A_CI.org[2] + 0 * 2.0, 1.0f);
            cameraLight->getLight()->setPosition(lightpos);
        }
        break;
    }

        /////////////////////////////////////////////////////////////////
        // Mouse button pressed events.
        /////////////////////////////////////////////////////////////////
    case (osgGA::GUIEventAdapter::PUSH):
    {
        // Left mouse button pressed
        if (ea.getButton() == osgGA::GUIEventAdapter::LEFT_MOUSE_BUTTON)
        {
            this->leftMouseButtonPressed = true;

            if (this->rightMouseButtonPressed)
            {
                double center[3] = {0.0, 0.0, 0.0};;
                osg::Node* click = getNodeUnderMouse(center);
                if (click)
                {
                    setTrackballCenter(center[0], center[1], center[2]);
                }
            }
        }
        // Right mouse button pressed
        else if (ea.getButton() == osgGA::GUIEventAdapter::RIGHT_MOUSE_BUTTON)
        {
            this->rightMouseButtonPressed = true;
        }

        break;
    }

        /////////////////////////////////////////////////////////////////
        // Mouse button released events.
        /////////////////////////////////////////////////////////////////
    case (osgGA::GUIEventAdapter::RELEASE):
    {

        // Left mouse button released.
        if (ea.getButton() == osgGA::GUIEventAdapter::LEFT_MOUSE_BUTTON)
        {
            this->leftMouseButtonPressed = false;
        }

        // Right mouse button released.
        if (ea.getButton() == osgGA::GUIEventAdapter::RIGHT_MOUSE_BUTTON)
        {
            this->rightMouseButtonPressed = false;
        }

        break;
    }

        /////////////////////////////////////////////////////////////////
        // Key pressed events
        /////////////////////////////////////////////////////////////////
    case (osgGA::GUIEventAdapter::KEYDOWN):
    {
        // key '0' is ASCII code 48, then running up to 57 for '9'
        if ((ea.getKey() >= 48) && (ea.getKey() <= 57))
        {
            unsigned int dLev = ea.getKey() - 48;
            RcsLogLevel = dLev;
            RMSG("Setting debug level to %u", dLev);
            return false;
        }

        else if (ea.getKey() == osgGA::GUIEventAdapter::KEY_F11)
        {
            HTr A_CI;
            double x[6];
            this->getCameraTransform(&A_CI);
            HTr_to6DVector(x, &A_CI);
            RMSGS("Camera pose is %f %f %f   %f %f %f   (degrees: %.3f %.3f %.3f)",
                  x[0], x[1], x[2], x[3], x[4], x[5],
                    RCS_RAD2DEG(x[3]), RCS_RAD2DEG(x[4]), RCS_RAD2DEG(x[5]));
        }

        //
        // Toggle wireframe
        //
        else if (ea.getKey() == 'w')
        {
            toggleWireframe();
            return false;
        }

        //
        // Toggle shadows
        //
        else if (ea.getKey() == 's')
        {
            setShadowEnabled(!this->shadowsEnabled);
            return false;
        }

        //
        // Toggle cartoon mode
        //
        else if (ea.getKey() == 'R')
        {
            // Once cartoon mode is enabled, other keys than R don't work
            // anymore. This is a OSG bug, because the effect node seems to
            // not call the children's eventhandlers
            this->cartoonEnabled = !this->cartoonEnabled;
            setCartoonEnabled(this->cartoonEnabled);
            return false;
        }
        else if (ea.getKey() == 'M')
        {
            keyHandler->toggleVideoCapture();
            return false;
        }

        break;
    }   // case(osgGA::GUIEventAdapter::KEYDOWN):

    default:
        break;

    }   // switch(ea.getEventType())

    return false;
}

/*******************************************************************************
 *
 ******************************************************************************/
bool Viewer::setTrackballCenter(double x, double y, double z)
{
    osgGA::TrackballManipulator* trackball =
            dynamic_cast<osgGA::TrackballManipulator*>(viewer->getCameraManipulator());

    if (trackball)
    {
        trackball->setCenter(osg::Vec3(x, y, z));
        return true;
    }

    return false;
}

/*******************************************************************************
 *
 ******************************************************************************/
bool Viewer::getTrackballCenter(double pos[3]) const
{
    osgGA::TrackballManipulator* trackball =
            dynamic_cast<osgGA::TrackballManipulator*>(viewer->getCameraManipulator());

    if (trackball)
    {
        osg::Vec3d tbCenter = trackball->getCenter();
        Vec3d_set(pos, tbCenter.x(), tbCenter.y(), tbCenter.z());
        return true;
    }

    return false;
}

/*******************************************************************************
 *
 ******************************************************************************/
bool Viewer::isThreadStopped() const
{
    return this->threadStopped;
}

}   // namespace Rcs
