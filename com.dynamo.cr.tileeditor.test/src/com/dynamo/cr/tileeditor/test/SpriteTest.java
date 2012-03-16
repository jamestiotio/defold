package com.dynamo.cr.tileeditor.test;

import static org.hamcrest.CoreMatchers.is;
import static org.junit.Assert.assertThat;
import static org.junit.Assert.assertTrue;

import java.io.IOException;

import org.eclipse.core.commands.ExecutionException;
import org.eclipse.core.runtime.CoreException;
import org.eclipse.core.runtime.IStatus;
import org.eclipse.osgi.util.NLS;
import org.junit.Before;
import org.junit.Test;

import com.dynamo.cr.sceneed.core.test.AbstractNodeTest;
import com.dynamo.cr.tileeditor.scene.Messages;
import com.dynamo.cr.tileeditor.scene.SpriteLoader;
import com.dynamo.cr.tileeditor.scene.SpriteNode;
import com.dynamo.cr.tileeditor.scene.TileSetLoader;
import com.dynamo.cr.tileeditor.scene.TileSetNode;
import com.dynamo.sprite.proto.Sprite.SpriteDesc;

public class SpriteTest extends AbstractNodeTest {

    private SpriteLoader loader;
    private SpriteNode spriteNode;

    @Override
    @Before
    public void setup() throws CoreException, IOException {
        super.setup();

        this.loader = new SpriteLoader();

        String[] paths = {"/test.tileset2", "/invalid.tileset2"};
        String[] contents = {
                "image: \"/2x5_16_1.png\" tile_width: 1 tile_height: 1 tile_margin: 0 tile_spacing: 0 material_tag: \"tile\"",
                "image: \"non_existant\" tile_width: 1 tile_height: 1 tile_margin: 0 tile_spacing: 0 material_tag: \"tile\""
        };
        for (int i = 0; i < paths.length; ++i) {
            String path = paths[i];
            registerFile(path, contents[i]);
            TileSetNode tileSet = new TileSetLoader().load(getLoaderContext(), getFile(path).getContents());
            tileSet.setModel(getModel());
            registerLoadedNode(path, tileSet);
        }

        this.spriteNode = registerAndLoadRoot(SpriteNode.class, "sprite", this.loader);
    }

    // Helpers

    private void setProperty(String id, Object value) throws ExecutionException {
        setNodeProperty(this.spriteNode, id, value);
    }

    private void assertPropertyStatus(String id, int severity, String message) {
        assertNodePropertyStatus(this.spriteNode, id, severity, message);
    }

    // Tests

    @Test
    public void testLoad() throws Exception {
        assertThat(this.spriteNode.getTileSet(), is(""));
        assertThat(this.spriteNode.getDefaultAnimation(), is("anim"));
    }

    private void create() throws Exception {
        setProperty("tileSet", "/test.tileset2");
        setProperty("defaultAnimation", "default");
    }

    @Test
    public void testBuildMessage() throws Exception {

        create();

        SpriteDesc ddf = (SpriteDesc)this.loader.buildMessage(getLoaderContext(), this.spriteNode, null);

        assertThat(ddf.getTileSet(), is(this.spriteNode.getTileSet()));
        assertThat(ddf.getDefaultAnimation(), is(this.spriteNode.getDefaultAnimation()));
    }

    @Test
    public void testReloadTileSet() throws Exception {
        create();

        assertTrue(this.spriteNode.handleReload(getFile("/test.tileset2")));
    }

    @Test
    public void testReloadTileSetImage() throws Exception {
        create();

        assertTrue(this.spriteNode.handleReload(getFile("/2x5_16_1.png")));
    }

    @Test
    public void testMessages() throws Exception {
        assertPropertyStatus("tileSet", IStatus.INFO, null); // default message

        setProperty("tileSet", "/non_existant");
        assertPropertyStatus("tileSet", IStatus.ERROR, null); // default message

        setProperty("tileSet", "/invalid.tileset2");
        assertPropertyStatus("tileSet", IStatus.ERROR, Messages.SpriteNode_tileSet_INVALID_REFERENCE);

        registerFile("/test.test", "");
        setProperty("tileSet", "/test.test");
        assertPropertyStatus("tileSet", IStatus.ERROR, Messages.SpriteNode_tileSet_CONTENT_ERROR);

        setProperty("defaultAnimation", "");
        assertPropertyStatus("defaultAnimation", IStatus.INFO, Messages.SpriteNode_defaultAnimation_EMPTY);

        setProperty("tileSet", "/test.tileset2");
        setProperty("defaultAnimation", "test");
        assertPropertyStatus("defaultAnimation", IStatus.ERROR, NLS.bind(Messages.SpriteNode_defaultAnimation_INVALID, "test"));
    }

}
