//////////////////////////////////////////////////////////////////////////
//
//  Copyright (c) 2018, Image Engine Design Inc. All rights reserved.
//
//  Redistribution and use in source and binary forms, with or without
//  modification, are permitted provided that the following conditions are
//  met:
//
//      * Redistributions of source code must retain the above
//        copyright notice, this list of conditions and the following
//        disclaimer.
//
//      * Redistributions in binary form must reproduce the above
//        copyright notice, this list of conditions and the following
//        disclaimer in the documentation and/or other materials provided with
//        the distribution.
//
//      * Neither the name of John Haddon nor the names of
//        any other contributors to this software may be used to endorse or
//        promote products derived from this software without specific prior
//        written permission.
//
//  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS
//  IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
//  THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
//  PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR
//  CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
//  EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
//  PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
//  PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
//  LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
//  NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
//  SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//
//////////////////////////////////////////////////////////////////////////

#include "GafferSceneUI/CameraTool.h"

#include "GafferSceneUI/SceneView.h"

#include "Gaffer/MetadataAlgo.h"
#include "Gaffer/ScriptNode.h"
#include "Gaffer/StringPlug.h"
#include "Gaffer/UndoScope.h"

#include "IECore/AngleConversion.h"

#include "OpenEXR/ImathEuler.h"
#include "OpenEXR/ImathMatrix.h"

#include "boost/algorithm/string/predicate.hpp"
#include "boost/bind.hpp"

using namespace std;
using namespace Imath;
using namespace IECore;
using namespace Gaffer;
using namespace GafferUI;
using namespace GafferScene;
using namespace GafferSceneUI;

IE_CORE_DEFINERUNTIMETYPED( CameraTool );

CameraTool::ToolDescription<CameraTool, SceneView> CameraTool::g_toolDescription;

size_t CameraTool::g_firstPlugIndex = 0;

CameraTool::CameraTool( SceneView *view, const std::string &name )
	:	SelectionTool( view, name ), m_dragId( 0 )
{
	storeIndexOfNextChild( g_firstPlugIndex );

	connectToViewContext();
	view->contextChangedSignal().connect( boost::bind( &CameraTool::connectToViewContext, this ) );

	view->plugDirtiedSignal().connect( boost::bind( &CameraTool::plugDirtied, this, ::_1 ) );
	plugDirtiedSignal().connect( boost::bind( &CameraTool::plugDirtied, this, ::_1 ) );

	// Snoop on the signals used for interaction with the viewport. We connect with group 0
	// so that we are called before everything else.
	view->viewportGadget()->dragBeginSignal().connect( 0, boost::bind( &CameraTool::viewportDragBegin, this, ::_2 ) );
	view->viewportGadget()->wheelSignal().connect( 0, boost::bind( &CameraTool::viewportWheel, this, ::_2 ) );
	view->viewportGadget()->keyPressSignal().connect( 0, boost::bind( &CameraTool::viewportKeyPress, this, ::_2 ) );
	view->viewportGadget()->buttonPressSignal().connect( 0, boost::bind( &CameraTool::viewportButtonPress, this, ::_2 ) );
	// Connect to `cameraChangedSignal()` so we can turn the viewport interaction into
	// camera edits in the node graph itself.
	m_viewportCameraChangedConnection = view->viewportGadget()->cameraChangedSignal().connect(
		boost::bind( &CameraTool::viewportCameraChanged, this )
	);

	// Connect to the preRender signal so we can coordinate ourselves with the work
	// that the SceneView::Camera class does to look through the camera we will be editing.
	view->viewportGadget()->preRenderSignal().connect( 0, boost::bind( &CameraTool::preRenderBegin, this ) );
	view->viewportGadget()->preRenderSignal().connect( boost::bind( &CameraTool::preRenderEnd, this ) );
}

CameraTool::~CameraTool()
{
}

const GafferScene::ScenePlug *CameraTool::scenePlug() const
{
	return view()->inPlug<ScenePlug>();
}

const Gaffer::BoolPlug *CameraTool::lookThroughEnabledPlug() const
{
	return view()->descendant<BoolPlug>( "camera.lookThroughEnabled" );
}

const Gaffer::StringPlug *CameraTool::lookThroughCameraPlug() const
{
	return view()->descendant<StringPlug>( "camera.lookThroughCamera" );
}

void CameraTool::connectToViewContext()
{
	m_contextChangedConnection = view()->getContext()->changedSignal().connect( boost::bind( &CameraTool::contextChanged, this, ::_2 ) );
}

void CameraTool::contextChanged( const IECore::InternedString &name )
{
	if( !boost::starts_with( name.string(), "ui:" ) )
	{
		m_cameraSelectionDirty = true;
		view()->viewportGadget()->renderRequestSignal()( view()->viewportGadget() );
	}
}

void CameraTool::plugDirtied( const Gaffer::Plug *plug )
{
	if(
		plug == activePlug() ||
		plug == scenePlug()->childNamesPlug() ||
		plug == scenePlug()->transformPlug() ||
		plug == scenePlug()->globalsPlug() ||
		plug == lookThroughEnabledPlug() ||
		plug == lookThroughCameraPlug()
	)
	{
		m_cameraSelectionDirty = true;
		view()->viewportGadget()->renderRequestSignal()( view()->viewportGadget() );
	}
}

GafferScene::ScenePlug::ScenePath CameraTool::cameraPath() const
{
	if( !activePlug()->getValue() )
	{
		return ScenePlug::ScenePath();
	}

	if( !lookThroughEnabledPlug()->getValue() )
	{
		return ScenePlug::ScenePath();
	}

	string cameraPath = lookThroughCameraPlug()->getValue();
	if( cameraPath.empty() )
	{
		Context::Scope scopedContext( view()->getContext() );
		IECore::ConstCompoundObjectPtr globals = view()->inPlug<ScenePlug>()->globals();
		if( auto *cameraData = globals->member<StringData>( "option:render:camera" ) )
		{
			cameraPath = cameraData->readable();
		}
	}

	ScenePlug::ScenePath result;
	ScenePlug::stringToPath( cameraPath, result );
	return result;
}

const TransformTool::Selection &CameraTool::cameraSelection()
{
	if( !m_cameraSelectionDirty )
	{
		return m_cameraSelection;
	}

	m_cameraSelection = TransformTool::Selection();
	ScenePlug::ScenePath cameraPath = this->cameraPath();
	if( !cameraPath.empty() )
	{
		m_cameraSelection = TransformTool::Selection(
			scenePlug(),
			cameraPath,
			view()->getContext()
		);
	}

	return m_cameraSelection;
}

void CameraTool::preRenderBegin()
{
	// The SceneView::Camera class updates the viewport camera
	// in `preRender()`, and we don't want to cause feedback by
	// trying to reflect that update back into the graph.
	/// \todo Should we have a more explicit synchronisation between
	/// SceneView::Camera and CameraTool?
	m_viewportCameraChangedConnection.block();
}

void CameraTool::preRenderEnd()
{
	const TransformTool::Selection &selection = cameraSelection();
	bool selectionEditable = false;
	if( selection.transformPlug )
	{
		selectionEditable = true;
		for( ValuePlugIterator it( selection.transformPlug->translatePlug() ); !it.done(); ++it )
		{
			if( !(*it)->settable() || MetadataAlgo::readOnly( it->get() ) )
			{
				selectionEditable = false;
				break;
			}
		}

		for( ValuePlugIterator it( selection.transformPlug->rotatePlug() ); !it.done(); ++it )
		{
			if( !(*it)->settable() || MetadataAlgo::readOnly( it->get() ) )
			{
				selectionEditable = false;
				break;
			}
		}
	}

	view()->viewportGadget()->setCameraEditable(
		!lookThroughEnabledPlug()->getValue() ||
		selectionEditable
	);

	if( selectionEditable )
	{
		view()->viewportGadget()->setCenterOfInterest(
			getCameraCenterOfInterest( selection.path )
		);
		m_viewportCameraChangedConnection.unblock();
	}
}

IECore::RunTimeTypedPtr CameraTool::viewportDragBegin( const GafferUI::DragDropEvent &event )
{
	// The viewport may be performing a camera drag. Set up our undo group
	// so that all the steps of the drag will be collapsed into a single undoable
	// block.
	m_undoGroup = boost::str( boost::format( "CameraTool%1%Drag%2%" ) % this % m_dragId++ );
	return nullptr;
}

bool CameraTool::viewportWheel( const GafferUI::ButtonEvent &event )
{
	// Merge all wheel events into a single undo.
	m_undoGroup = boost::str( boost::format( "CameraTool%1%Wheel" ) % this );
	return false;
}

bool CameraTool::viewportKeyPress( const GafferUI::KeyEvent &event )
{
	// Make sure we don't merge any edits into previous drag/wheel edits.
	m_undoGroup = "";
	return false;
}

bool CameraTool::viewportButtonPress( const GafferUI::ButtonEvent &event )
{
	// Make sure we don't merge any edits into previous drag/wheel edits.
	m_undoGroup = "";
	return false;
}

void CameraTool::viewportCameraChanged()
{
	const TransformTool::Selection &selection = cameraSelection();
	if( !selection.transformPlug )
	{
		return;
	}

	if( !view()->viewportGadget()->getCameraEditable() )
	{
		return;
	}

	const M44f viewportCameraTransform = view()->viewportGadget()->getCameraTransform();
	{
		Context::Scope scopedContext( selection.context.get() );
		if( selection.scene->transform( selection.path ) == viewportCameraTransform )
		{
			return;
		}
	}

	const M44f transformSpaceMatrix = viewportCameraTransform * selection.sceneToTransformSpace();

	Eulerf e; e.extract( transformSpaceMatrix );
	e.makeNear( degreesToRadians( selection.transformPlug->rotatePlug()->getValue() ) );
	const V3f r = radiansToDegrees( V3f( e ) );

	UndoScope undoScope( selection.transformPlug->ancestor<ScriptNode>(), UndoScope::Enabled, m_undoGroup );

	selection.transformPlug->rotatePlug()->setValue( r );
	selection.transformPlug->translatePlug()->setValue( transformSpaceMatrix.translation() );

	// Create an action to save/restore the current center of interest, so that
	// when the user undos a framing action, they get back to the old center of
	// interest as well as the old transform.
	Action::enact(
		selection.transformPlug,
		// Do
		boost::bind(
			&CameraTool::setCameraCenterOfInterest,
			CameraToolPtr( this ), selection.path,
			view()->viewportGadget()->getCenterOfInterest()
		),
		// Undo
		boost::bind(
			&CameraTool::setCameraCenterOfInterest,
			CameraToolPtr( this ), selection.path,
			getCameraCenterOfInterest( selection.path )
		)
	);
}

void CameraTool::setCameraCenterOfInterest( const GafferScene::ScenePlug::ScenePath &camera, float centerOfInterest )
{
	string key;
	ScenePlug::pathToString( camera, key );
	m_cameraCentersOfInterest[key] = centerOfInterest;
}

float CameraTool::getCameraCenterOfInterest( const GafferScene::ScenePlug::ScenePath &camera ) const
{
	string key;
	ScenePlug::pathToString( camera, key );
	CameraCentersOfInterest::const_iterator it = m_cameraCentersOfInterest.find( key );
	if( it != m_cameraCentersOfInterest.end() )
	{
		return it->second;
	}
	else
	{
		return 1.0f;
	}
}
