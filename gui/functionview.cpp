#include "functionview.hpp"

#include <QHBoxLayout>

#include <KMessageBox>

#include "../src/Node.hpp"
#include "../src/NodeGraphicsObject.hpp"

#include <chig/JsonModule.hpp>
#include <chig/NodeInstance.hpp>

#include "chignodegui.hpp"

FunctionView::FunctionView(
	chig::GraphFunction* func_, QWidget* parent)
	: QWidget(parent), func{func_}
{
	auto hlayout = new QHBoxLayout(this);

	hlayout->setMargin(0);
	hlayout->setSpacing(0);
	
	// create the registry
	//////////////////////
	
	auto reg = std::make_shared<DataModelRegistry>();
	
	// register dependencies + our own mod
	auto deps =  func->module().dependencies();
	deps.insert(func->module().fullName());
	for(auto modName : deps) {
		auto module = func->context().moduleByFullName(modName);
		Expects(module != nullptr);
		
		for(auto typeName : module->nodeTypeNames()) {
			
			// create that node type unless it's entry or exit
			if(modName == "lang" && (typeName == "entry" || typeName == "exit")) {
				continue;
			}
			
			std::unique_ptr<chig::NodeType> ty;
			module->nodeTypeFromName(typeName, {}, &ty);
			
			auto name = ty->qualifiedName(); // cache the name because ty is moved from
			reg->registerModel(std::make_unique<ChigNodeGui>(new chig::NodeInstance(std::move(ty), 0, 0, name)));
			
		}
	}
	// register functions in this module
	// register exit -- it has to be the speical kind of exit for this function
	std::unique_ptr<chig::NodeType> ty;
	func->createExitNodeType(&ty);
	
	reg->registerModel(std::make_unique<ChigNodeGui>(new chig::NodeInstance(std::move(ty), 0, 0, "lang:exit")));

	

	scene = new FlowScene(reg);
	connect(scene, &FlowScene::nodeCreated, this, &FunctionView::nodeAdded);
	connect(scene, &FlowScene::nodeDeleted, this, &FunctionView::nodeDeleted);

	connect(scene, &FlowScene::connectionCreated, this, &FunctionView::connectionAdded);
	connect(scene, &FlowScene::connectionDeleted, this, &FunctionView::connectionDeleted);

	view = new FlowView(scene);

	hlayout->addWidget(view);

	// create nodes
	for (auto& node : func->graph().nodes()) {
		std::shared_ptr<Node> guinode =
			scene->createNode(std::make_unique<ChigNodeGui>(node.second.get()));

		guinode->nodeGraphicsObject().setPos({node.second->x(), node.second->y()});

		nodes[node.second.get()] = guinode;
	}

	// create connections
	for (auto& node : func->graph().nodes()) {
		auto thisNode = nodes[node.second.get()].lock();

		size_t connId = 0;
		for (auto& conn : node.second->inputDataConnections) {
			if (conn.first == nullptr) {
				continue;
			}
			auto inData = nodes[conn.first].lock();

			auto guiconn =
				scene
					->createConnection(thisNode, connId + node.second->inputExecConnections.size(),
						inData, conn.second + conn.first->outputExecConnections.size())
					.get();

			conns[guiconn] = {{{conn.first, conn.second + conn.first->outputExecConnections.size()},
				{node.second.get(), connId + node.second->inputExecConnections.size()}}};

			++connId;
		}

		connId = 0;
		for (auto& conn : node.second->outputExecConnections) {
			auto outExecNode = nodes[conn.first].lock();

			if (outExecNode) {
				auto guiconn =
					scene->createConnection(outExecNode, conn.second, thisNode, connId).get();

				conns[guiconn] = {{{node.second.get(), connId}, {conn.first, conn.second}}};
			}
			++connId;
		}
	}

	creating = false;
}

void FunctionView::nodeAdded(const std::shared_ptr<Node>& n)
{
	if (creating) {
		return;
	}

	auto ptr = dynamic_cast<ChigNodeGui*>(n->nodeDataModel());

	if (ptr == nullptr) {
		return;
	}

	func->graph().nodes()[ptr->inst->id()] = std::unique_ptr<chig::NodeInstance>(ptr->inst);
    
    nodes[ptr->inst] = n;
}

void FunctionView::nodeDeleted(const std::shared_ptr<Node>& n)
{
	auto ptr = dynamic_cast<ChigNodeGui*>(n->nodeDataModel());

	if (ptr == nullptr) {
		return;
	}
	
	// find connections to this in conns and delete them -- removeNode does this in the chigraph model now do that in the nodes model
	
	for(auto iter = conns.begin(); iter != conns.end();) {
        auto& pair = *iter;
        if(pair.second[0].first == ptr->inst || pair.second[1].first == ptr->inst) {
            // this doesn't invalidate iterators don't worry 
            // http://en.cppreference.com/w/cpp/container/unordered_map#Iterator_invalidation
            conns.erase(iter);
            
            iter = conns.begin(); // reset our search so we don't miss anything, because iter was invalidated
        } else {
            ++iter; // only go to the next one if we don't go back to the beginning
        }
    }

    func->removeNode(ptr->inst);
    
    nodes.erase(ptr->inst);
}

void FunctionView::connectionAdded(const Connection& c)
{
	if (creating) {
		return;
	}

	std::shared_ptr<Node> lguinode, rguinode;
	lguinode = c.getNode(PortType::Out).lock();
	rguinode = c.getNode(PortType::In).lock();

	connect(&c, &Connection::updated, this, &FunctionView::connectionUpdated);

	if (!lguinode || !rguinode) {
		return;
	}

	// here, in and out mean input and output to the connection (like in chigraph)
	auto outptr = dynamic_cast<ChigNodeGui*>(rguinode->nodeDataModel());
	auto inptr = dynamic_cast<ChigNodeGui*>(lguinode->nodeDataModel());

	if (outptr == nullptr || inptr == nullptr) {
		return;
	}

	size_t inconnid = c.getPortIndex(PortType::Out);
	size_t outconnid = c.getPortIndex(PortType::In);

	conns[&c] = std::array<std::pair<chig::NodeInstance*, size_t>, 2>{
		{std::make_pair(inptr->inst, inconnid), std::make_pair(outptr->inst, outconnid)}};

	bool isExec = inconnid < inptr->inst->type().execOutputs().size();

	if (isExec) {
		chig::connectExec(*inptr->inst, inconnid, *outptr->inst, outconnid);
	} else {
		chig::connectData(*inptr->inst, inconnid - inptr->inst->type().execOutputs().size(),
			*outptr->inst, outconnid - outptr->inst->type().execInputs().size());
	}
}
void FunctionView::connectionDeleted(Connection& c)
{
	auto conniter = conns.find(&c);
	if (conniter == conns.end()) {
		return;
	}
	
	// don't do anything if

	auto conn = conniter->second;

	// see if it's data or exec
	bool isExec = conn[0].second < conn[0].first->outputExecConnections.size();

	chig::Result res;

	if (isExec) {
		res += chig::disconnectExec(*conn[0].first, conn[0].second);
	} else {
		res += chig::disconnectData(*conn[0].first,
			conn[0].second - conn[0].first->outputExecConnections.size(), *conn[1].first);
	}

	if (!res) {
		KMessageBox::detailedError(
			this, "Internal error deleting connection", QString::fromStdString(res.dump()));
	}

	conns.erase(&c);
}

void FunctionView::updatePositions()
{
	for (auto& inst : nodes) {
		auto sptr = inst.second.lock();
		if (sptr) {
			QPointF pos = sptr->nodeGraphicsObject().pos();
			inst.first->setX(pos.x());
			inst.first->setY(pos.y());
		}
	}
}

void FunctionView::connectionUpdated(const Connection& c)
{
	if (creating) return;

	// find in assoc
	auto iter = conns.find(&c);
	if (iter == conns.end()) {
		return connectionAdded(c);
	}

	// remove the existing connection

}
