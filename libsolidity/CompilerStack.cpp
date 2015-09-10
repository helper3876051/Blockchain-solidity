/*
	This file is part of cpp-ethereum.

	cpp-ethereum is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	cpp-ethereum is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with cpp-ethereum.  If not, see <http://www.gnu.org/licenses/>.
*/
/**
 * @author Christian <c@ethdev.com>
 * @author Gav Wood <g@ethdev.com>
 * @date 2014
 * Full-stack compiler that converts a source code string to bytecode.
 */

#include <boost/algorithm/string.hpp>
#include <libsolidity/AST.h>
#include <libsolidity/Scanner.h>
#include <libsolidity/Parser.h>
#include <libsolidity/GlobalContext.h>
#include <libsolidity/NameAndTypeResolver.h>
#include <libsolidity/Compiler.h>
#include <libsolidity/CompilerStack.h>
#include <libsolidity/InterfaceHandler.h>

#include <libdevcore/SHA3.h>

using namespace std;

namespace dev
{
namespace solidity
{

const map<string, string> StandardSources = map<string, string>{
	{"coin", R"(import "CoinReg";import "Config";import "configUser";contract coin is configUser{function coin(bytes3 name, uint denom) {CoinReg(Config(configAddr()).lookup(3)).register(name, denom);}})"},
	{"Coin", R"(contract Coin{function isApprovedFor(address _target,address _proxy)constant returns(bool _r){}function isApproved(address _proxy)constant returns(bool _r){}function sendCoinFrom(address _from,uint256 _val,address _to){}function coinBalanceOf(address _a)constant returns(uint256 _r){}function sendCoin(uint256 _val,address _to){}function coinBalance()constant returns(uint256 _r){}function approve(address _a){}})"},
	{"CoinReg", R"(contract CoinReg{function count()constant returns(uint256 r){}function info(uint256 i)constant returns(address addr,bytes3 name,uint256 denom){}function register(bytes3 name,uint256 denom){}function unregister(){}})"},
	{"configUser", R"(contract configUser{function configAddr()constant returns(address a){ return 0xc6d9d2cd449a754c494264e1809c50e34d64562b;}})"},
	{"Config", R"(contract Config{function lookup(uint256 service)constant returns(address a){}function kill(){}function unregister(uint256 id){}function register(uint256 id,address service){}})"},
	{"mortal", R"(import "owned";contract mortal is owned {function kill() { if (msg.sender == owner) suicide(owner); }})"},
	{"named", R"(import "Config";import "NameReg";import "configUser";contract named is configUser {function named(bytes32 name) {NameReg(Config(configAddr()).lookup(1)).register(name);}})"},
	{"NameReg", R"(contract NameReg{function register(bytes32 name){}function addressOf(bytes32 name)constant returns(address addr){}function unregister(){}function nameOf(address addr)constant returns(bytes32 name){}})"},
	{"owned", R"(contract owned{function owned(){owner = msg.sender;}modifier onlyowner(){if(msg.sender==owner)_}address owner;})"},
	{"service", R"(import "Config";import "configUser";contract service is configUser{function service(uint _n){Config(configAddr()).register(_n, this);}})"},
	{"std", R"(import "owned";import "mortal";import "Config";import "configUser";import "NameReg";import "named";)"}
};

CompilerStack::CompilerStack(bool _addStandardSources):
	m_parseSuccessful(false)
{
	if (_addStandardSources)
		addSources(StandardSources, true); // add them as libraries
}

void CompilerStack::reset(bool _keepSources, bool _addStandardSources)
{
	m_parseSuccessful = false;
	if (_keepSources)
		for (auto sourcePair: m_sources)
			sourcePair.second.reset();
	else
	{
		m_sources.clear();
		if (_addStandardSources)
			addSources(StandardSources, true);
	}
	m_globalContext.reset();
	m_sourceOrder.clear();
	m_contracts.clear();
}

bool CompilerStack::addSource(string const& _name, string const& _content, bool _isLibrary)
{
	bool existed = m_sources.count(_name) != 0;
	reset(true);
	m_sources[_name].scanner = make_shared<Scanner>(CharStream(_content), _name);
	m_sources[_name].isLibrary = _isLibrary;
	return existed;
}

void CompilerStack::setSource(string const& _sourceCode)
{
	reset();
	addSource("", _sourceCode);
}

void CompilerStack::parse()
{
	for (auto& sourcePair: m_sources)
	{
		sourcePair.second.scanner->reset();
		sourcePair.second.ast = Parser().parse(sourcePair.second.scanner);
	}
	resolveImports();

	m_globalContext = make_shared<GlobalContext>();
	NameAndTypeResolver resolver(m_globalContext->declarations());
	for (Source const* source: m_sourceOrder)
		resolver.registerDeclarations(*source->ast);
	for (Source const* source: m_sourceOrder)
		for (ASTPointer<ASTNode> const& node: source->ast->nodes())
			if (ContractDefinition* contract = dynamic_cast<ContractDefinition*>(node.get()))
			{
				m_globalContext->setCurrentContract(*contract);
				resolver.updateDeclaration(*m_globalContext->currentThis());
				resolver.updateDeclaration(*m_globalContext->currentSuper());
				resolver.resolveNamesAndTypes(*contract);
				m_contracts[contract->name()].contract = contract;
			}
	InterfaceHandler interfaceHandler;
	for (Source const* source: m_sourceOrder)
		for (ASTPointer<ASTNode> const& node: source->ast->nodes())
			if (ContractDefinition* contract = dynamic_cast<ContractDefinition*>(node.get()))
			{
				m_globalContext->setCurrentContract(*contract);
				resolver.updateDeclaration(*m_globalContext->currentThis());
				resolver.checkTypeRequirements(*contract);
				contract->setDevDocumentation(interfaceHandler.devDocumentation(*contract));
				contract->setUserDocumentation(interfaceHandler.userDocumentation(*contract));
				m_contracts[contract->name()].contract = contract;
			}
	m_parseSuccessful = true;
}

void CompilerStack::parse(string const& _sourceCode)
{
	setSource(_sourceCode);
	parse();
}

vector<string> CompilerStack::contractNames() const
{
	if (!m_parseSuccessful)
		BOOST_THROW_EXCEPTION(CompilerError() << errinfo_comment("Parsing was not successful."));
	vector<string> contractNames;
	for (auto const& contract: m_contracts)
		contractNames.push_back(contract.first);
	return contractNames;
}


void CompilerStack::compile(bool _optimize, unsigned _runs)
{
	if (!m_parseSuccessful)
		parse();

	map<ContractDefinition const*, eth::Assembly const*> compiledContracts;
	for (Source const* source: m_sourceOrder)
		for (ASTPointer<ASTNode> const& node: source->ast->nodes())
			if (ContractDefinition* contract = dynamic_cast<ContractDefinition*>(node.get()))
			{
				if (!contract->isFullyImplemented())
					continue;
				shared_ptr<Compiler> compiler = make_shared<Compiler>(_optimize, _runs);
				compiler->compileContract(*contract, compiledContracts);
				Contract& compiledContract = m_contracts.at(contract->name());
				compiledContract.compiler = compiler;
				compiledContract.object = compiler->assembledObject();
				compiledContract.runtimeObject = compiler->runtimeObject();
				compiledContracts[compiledContract.contract] = &compiler->assembly();

				Compiler cloneCompiler(_optimize, _runs);
				cloneCompiler.compileClone(*contract, compiledContracts);
				compiledContract.cloneObject = cloneCompiler.assembledObject();
			}
}

eth::LinkerObject const& CompilerStack::compile(string const& _sourceCode, bool _optimize)
{
	parse(_sourceCode);
	compile(_optimize);
	return object();
}

eth::AssemblyItems const* CompilerStack::assemblyItems(string const& _contractName) const
{
	Contract const& currentContract = contract(_contractName);
	return currentContract.compiler ? &contract(_contractName).compiler->assemblyItems() : nullptr;
}

eth::AssemblyItems const* CompilerStack::runtimeAssemblyItems(string const& _contractName) const
{
	Contract const& currentContract = contract(_contractName);
	return currentContract.compiler ? &contract(_contractName).compiler->runtimeAssemblyItems() : nullptr;
}

eth::LinkerObject const& CompilerStack::object(string const& _contractName) const
{
	return contract(_contractName).object;
}

eth::LinkerObject const& CompilerStack::runtimeObject(string const& _contractName) const
{
	return contract(_contractName).runtimeObject;
}

eth::LinkerObject const& CompilerStack::cloneObject(string const& _contractName) const
{
	return contract(_contractName).cloneObject;
}

dev::h256 CompilerStack::contractCodeHash(string const& _contractName) const
{
	auto const& obj = runtimeObject(_contractName);
	if (obj.bytecode.empty() || !obj.linkReferences.empty())
		return dev::h256();
	else
		return dev::sha3(obj.bytecode);
}

Json::Value CompilerStack::streamAssembly(ostream& _outStream, string const& _contractName, StringMap _sourceCodes, bool _inJsonFormat) const
{
	Contract const& currentContract = contract(_contractName);
	if (currentContract.compiler)
		return currentContract.compiler->streamAssembly(_outStream, _sourceCodes, _inJsonFormat);
	else
	{
		_outStream << "Contract not fully implemented" << endl;
		return Json::Value();
	}
}

string const& CompilerStack::interface(string const& _contractName) const
{
	return metadata(_contractName, DocumentationType::ABIInterface);
}

string const& CompilerStack::solidityInterface(string const& _contractName) const
{
	return metadata(_contractName, DocumentationType::ABISolidityInterface);
}

string const& CompilerStack::metadata(string const& _contractName, DocumentationType _type) const
{
	if (!m_parseSuccessful)
		BOOST_THROW_EXCEPTION(CompilerError() << errinfo_comment("Parsing was not successful."));

	Contract const& currentContract = contract(_contractName);

	std::unique_ptr<string const>* doc;

	// checks wheather we already have the documentation
	switch (_type)
	{
	case DocumentationType::NatspecUser:
		doc = &currentContract.userDocumentation;
		break;
	case DocumentationType::NatspecDev:
		doc = &currentContract.devDocumentation;
		break;
	case DocumentationType::ABIInterface:
		doc = &currentContract.interface;
		break;
	case DocumentationType::ABISolidityInterface:
		doc = &currentContract.solidityInterface;
		break;
	default:
		BOOST_THROW_EXCEPTION(InternalCompilerError() << errinfo_comment("Illegal documentation type."));
	}

	// caches the result
	if (!*doc)
		doc->reset(new string(currentContract.interfaceHandler->documentation(*currentContract.contract, _type)));

	return *(*doc);
}

Scanner const& CompilerStack::scanner(string const& _sourceName) const
{
	return *source(_sourceName).scanner;
}

SourceUnit const& CompilerStack::ast(string const& _sourceName) const
{
	return *source(_sourceName).ast;
}

ContractDefinition const& CompilerStack::contractDefinition(string const& _contractName) const
{
	return *contract(_contractName).contract;
}

size_t CompilerStack::functionEntryPoint(
	std::string const& _contractName,
	FunctionDefinition const& _function
) const
{
	shared_ptr<Compiler> const& compiler = contract(_contractName).compiler;
	if (!compiler)
		return 0;
	eth::AssemblyItem tag = compiler->functionEntryLabel(_function);
	if (tag.type() == eth::UndefinedItem)
		return 0;
	eth::AssemblyItems const& items = compiler->runtimeAssemblyItems();
	for (size_t i = 0; i < items.size(); ++i)
		if (items.at(i).type() == eth::Tag && items.at(i).data() == tag.data())
			return i;
	return 0;
}

eth::LinkerObject CompilerStack::staticCompile(std::string const& _sourceCode, bool _optimize)
{
	CompilerStack stack;
	return stack.compile(_sourceCode, _optimize);
}

tuple<int, int, int, int> CompilerStack::positionFromSourceLocation(SourceLocation const& _sourceLocation) const
{
	int startLine;
	int startColumn;
	int endLine;
	int endColumn;
	tie(startLine, startColumn) = scanner(*_sourceLocation.sourceName).translatePositionToLineColumn(_sourceLocation.start);
	tie(endLine, endColumn) = scanner(*_sourceLocation.sourceName).translatePositionToLineColumn(_sourceLocation.end);

	return make_tuple(++startLine, ++startColumn, ++endLine, ++endColumn);
}

void CompilerStack::resolveImports()
{
	// topological sorting (depth first search) of the import graph, cutting potential cycles
	vector<Source const*> sourceOrder;
	set<Source const*> sourcesSeen;

	function<void(Source const*)> toposort = [&](Source const* _source)
	{
		if (sourcesSeen.count(_source))
			return;
		sourcesSeen.insert(_source);
		for (ASTPointer<ASTNode> const& node: _source->ast->nodes())
			if (ImportDirective const* import = dynamic_cast<ImportDirective*>(node.get()))
			{
				string const& id = import->identifier();
				if (!m_sources.count(id))
					BOOST_THROW_EXCEPTION(
						ParserError()
							  << errinfo_sourceLocation(import->location())
							  << errinfo_comment("Source not found.")
					);
				toposort(&m_sources[id]);
			}
		sourceOrder.push_back(_source);
	};

	for (auto const& sourcePair: m_sources)
		if (!sourcePair.second.isLibrary)
			toposort(&sourcePair.second);

	swap(m_sourceOrder, sourceOrder);
}

std::string CompilerStack::defaultContractName() const
{
	return contract("").contract->name();
}

CompilerStack::Contract const& CompilerStack::contract(string const& _contractName) const
{
	if (m_contracts.empty())
		BOOST_THROW_EXCEPTION(CompilerError() << errinfo_comment("No compiled contracts found."));
	string contractName = _contractName;
	if (_contractName.empty())
		// try to find some user-supplied contract
		for (auto const& it: m_sources)
			if (!StandardSources.count(it.first))
				for (ASTPointer<ASTNode> const& node: it.second.ast->nodes())
					if (auto contract = dynamic_cast<ContractDefinition const*>(node.get()))
						contractName = contract->name();
	auto it = m_contracts.find(contractName);
	if (it == m_contracts.end())
		BOOST_THROW_EXCEPTION(CompilerError() << errinfo_comment("Contract " + _contractName + " not found."));
	return it->second;
}

CompilerStack::Source const& CompilerStack::source(string const& _sourceName) const
{
	auto it = m_sources.find(_sourceName);
	if (it == m_sources.end())
		BOOST_THROW_EXCEPTION(CompilerError() << errinfo_comment("Given source file not found."));
	return it->second;
}

CompilerStack::Contract::Contract(): interfaceHandler(make_shared<InterfaceHandler>()) {}

}
}
